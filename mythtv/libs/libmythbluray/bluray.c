/*
 * This file is part of libbluray
 * Copyright (C) 2009-2010  Obliter0n
 * Copyright (C) 2009-2010  John Stebbins
 * Copyright (C) 2010       hpi1
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library. If not, see
 * <http://www.gnu.org/licenses/>.
 */

#if HAVE_CONFIG_H
#include "config.h"
#endif

#include "bluray.h"
#include "register.h"
#include "util/macro.h"
#include "util/logging.h"
#include "util/strutl.h"
#include "bdnav/navigation.h"
#include "bdnav/index_parse.h"
#include "bdnav/meta_parse.h"
#include "bdnav/clpi_parse.h"
#include "hdmv/hdmv_vm.h"
#include "decoders/graphics_controller.h"
#include "file/file.h"
#ifdef DLOPEN_CRYPTO_LIBS
#include "file/dl.h"
#endif
#ifdef USING_BDJAVA
#include "bdj/bdj.h"
#endif

#ifndef DLOPEN_CRYPTO_LIBS
#include <libaacs/aacs.h>
#include <libbdplus/bdplus.h>
#endif
#include <stdlib.h>
#include <inttypes.h>
#include <string.h>
#include <sys/types.h>

#include "mythiowrapper.h"

typedef int     (*fptr_int)();
typedef int32_t (*fptr_int32)();
typedef void*   (*fptr_p_void)();

#define MAX_EVENTS 31  /* 2^n - 1 */
typedef struct bd_event_queue_s {
    unsigned in;  /* next free slot */
    unsigned out; /* next event */
    BD_EVENT ev[MAX_EVENTS+1];
} BD_EVENT_QUEUE;

typedef enum {
    title_undef = 0,
    title_hdmv,
    title_bdj,
} BD_TITLE_TYPE;

typedef struct {
    /* current clip */
    NAV_CLIP       *clip;
    BD_FILE_H      *fp;
    uint64_t       clip_size;
    uint64_t       clip_block_pos;
    uint64_t       clip_pos;

    /* current aligned unit */
    uint16_t       int_buf_off;

    BD_UO_MASK     uo_mask;

} BD_STREAM;

typedef struct {
    NAV_CLIP *clip;
    uint64_t  clip_size;
    uint8_t  *buf;
} BD_PRELOAD;

struct bluray {

    /* current disc */
    char             *device_path;
    BLURAY_DISC_INFO  disc_info;
    INDX_ROOT        *index;
    META_ROOT        *meta;
    NAV_TITLE_LIST   *title_list;

    /* current playlist */
    NAV_TITLE      *title;
    uint32_t       title_idx;
    uint64_t       s_pos;

    /* streams */
    BD_STREAM      st0; /* main path */
    BD_PRELOAD     st_ig; /* preloaded IG stream sub path */

    /* buffer for bd_read(): current aligned unit of main stream (st0) */
    uint8_t        int_buf[6144];

    /* seamless angle change request */
    int            seamless_angle_change;
    uint32_t       angle_change_pkt;
    uint32_t       angle_change_time;
    unsigned       request_angle;

    /* chapter tracking */
    uint64_t       next_chapter_start;

    /* aacs */
#ifdef DLOPEN_CRYPTO_LIBS
    void           *h_libaacs;   // library handle
#endif
    void           *aacs;
    fptr_p_void    libaacs_open;
    fptr_int       libaacs_decrypt_unit;

    /* BD+ */
#ifdef DLOPEN_CRYPTO_LIBS
    void           *h_libbdplus; // library handle
#endif
    void           *bdplus;
    fptr_p_void    bdplus_init;
    fptr_int32     bdplus_seek;
    fptr_int32     bdplus_fixup;

    /* player state */
    BD_REGISTERS   *regs;       // player registers
    BD_EVENT_QUEUE *event_queue; // navigation mode event queue
    BD_TITLE_TYPE  title_type;  // type of current title (in navigation mode)

    HDMV_VM        *hdmv_vm;
    uint8_t        hdmv_suspended;

    void           *bdjava;

    /* graphics */
    GRAPHICS_CONTROLLER *graphics_controller;
};

#ifdef DLOPEN_CRYPTO_LIBS
#    define DL_CALL(lib,func,param,...)             \
     do {                                           \
          fptr_p_void fptr = (fptr_p_void)dl_dlsym(lib, #func);  \
          if (fptr) {                               \
              fptr(param, ##__VA_ARGS__);           \
          }                                         \
      } while (0)
#else
#    define DL_CALL(lib,func,param,...)         \
     func (param, ##__VA_ARGS__)
#endif

/*
 * Navigation mode event queue
 */

static void _init_event_queue(BLURAY *bd)
{
    if (!bd->event_queue) {
        bd->event_queue = calloc(1, sizeof(struct bd_event_queue_s));
    } else {
        memset(bd->event_queue, 0, sizeof(struct bd_event_queue_s));
    }
}

static int _get_event(BLURAY *bd, BD_EVENT *ev)
{
    struct bd_event_queue_s *eq = bd->event_queue;

    if (eq) {
        if (eq->in != eq->out) {
            *ev = eq->ev[eq->out];
            eq->out = (eq->out + 1) & MAX_EVENTS;
            return 1;
        }
    }

    ev->event = BD_EVENT_NONE;

    return 0;
}

static int _queue_event(BLURAY *bd, BD_EVENT ev)
{
    struct bd_event_queue_s *eq = bd->event_queue;

    if (eq) {
        unsigned new_in = (eq->in + 1) & MAX_EVENTS;

        if (new_in != eq->out) {
            eq->ev[eq->in] = ev;
            eq->in = new_in;
            return 1;
        }

        BD_DEBUG(DBG_BLURAY|DBG_CRIT, "_queue_event(%d, %d): queue overflow !\n", ev.event, ev.param);
    }

    return 0;
}

/*
+ * PSR utils
+ */

static void _update_stream_psr_by_lang(BD_REGISTERS *regs,
                                       uint32_t psr_lang, uint32_t psr_stream,
                                       uint32_t enable_flag, uint32_t undefined_val,
                                       MPLS_STREAM *streams, unsigned num_streams)
{
    uint32_t psr_val;
    int      stream_idx = -1;
    unsigned ii;

    /* get preferred language */
    psr_val = bd_psr_read(regs, psr_lang);
    if (psr_val == 0xffffff) {
        /* language setting not initialized */
        return;
    }

    /* find stream */

    for (ii = 0; ii < num_streams; ii++) {
        if (psr_val == str_to_uint32((const char *)streams[ii].lang, 3)) {
            stream_idx = ii;
            break;
        }
    }

    if (stream_idx < 0) {
        /* requested language not found */
        stream_idx = undefined_val - 1;
        enable_flag = 0;
    }
    /* update PSR */

    BD_DEBUG(DBG_BLURAY, "Selected stream %d (language %s)\n", ii, streams[ii].lang);

    bd_psr_lock(regs);

    psr_val = bd_psr_read(regs, psr_stream) & 0xffff0000;
    psr_val |= (stream_idx + 1) | enable_flag;
    bd_psr_write(regs, psr_stream, psr_val);

    bd_psr_unlock(regs);
}

static void _update_clip_psrs(BLURAY *bd, NAV_CLIP *clip)
{
    bd_psr_write(bd->regs, PSR_PLAYITEM, clip->ref);
    bd_psr_write(bd->regs, PSR_TIME,     clip->in_time);

    /* Update selected audio and subtitle stream PSRs when not using menus.
     * Selection is based on language setting PSRs and clip STN.
     */
    if (bd->title_type == title_undef) {
        MPLS_STN *stn = &clip->title->pl->play_item[clip->ref].stn;

        _update_stream_psr_by_lang(bd->regs,
                                   PSR_AUDIO_LANG, PSR_PRIMARY_AUDIO_ID, 0, 0xff,
                                   stn->audio, stn->num_audio);
        _update_stream_psr_by_lang(bd->regs,
                                   PSR_PG_AND_SUB_LANG, PSR_PG_STREAM, 0x80000000, 0xfff,
                                   stn->pg, stn->num_pg);
    }
}


static void _update_chapter_psr(BLURAY *bd)
{
  uint32_t current_chapter = bd_get_current_chapter(bd);
  bd->next_chapter_start = bd_chapter_pos(bd, current_chapter + 1);
  bd_psr_write(bd->regs, PSR_CHAPTER,  current_chapter + 1);
}

/*
 * clip access (BD_STREAM)
 */

static void _close_m2ts(BD_STREAM *st)
{
    if (st->fp != NULL) {
        file_close(st->fp);
        st->fp = NULL;
    }

    /* reset UO mask */
    memset(&st->uo_mask, 0, sizeof(st->uo_mask));
}

static int _open_m2ts(BLURAY *bd, BD_STREAM *st)
{
    char *f_name;
    struct stat buf;

    _close_m2ts(st);

    f_name = str_printf("%s" DIR_SEP "BDMV" DIR_SEP "STREAM" DIR_SEP "%s",
                        bd->device_path, st->clip->name);

    st->clip_pos = (uint64_t)st->clip->start_pkt * 192;
    st->clip_block_pos = (st->clip_pos / 6144) * 6144;

    if ((st->fp = file_open(f_name, "rb"))) {
// Original libbluray code
//        file_seek(st->fp, 0, SEEK_END);
//        if ((st->clip_size = file_tell(st->fp))) {
// New 'stat' and modified 'if' to minimize RingBuffer seeking
// Optimize here for now until we can optimize in the RingBuffer itself
        if (mythfile_stat(f_name, &buf) == 0)
            st->clip_size = buf.st_size;
        else
            st->clip_size = 0;

        if (st->clip_size) {
            file_seek(st->fp, st->clip_block_pos, SEEK_SET);
            st->int_buf_off = 6144;
            X_FREE(f_name);

            if (bd->bdplus) {
                DL_CALL(bd->h_libbdplus, bdplus_set_title,
                        bd->bdplus, st->clip->clip_id);
            }

            if (bd->aacs) {
                uint32_t title = bd_psr_read(bd->regs, PSR_TITLE_NUMBER);
                DL_CALL(bd->h_libaacs, aacs_select_title,
                        bd->aacs, title);
            }

            if (st == &bd->st0) {
                MPLS_PL *pl = st->clip->title->pl;
                st->uo_mask = bd_uo_mask_combine(pl->app_info.uo_mask,
                                                 pl->play_item[st->clip->ref].uo_mask);

                _update_clip_psrs(bd, st->clip);
            }

            return 1;
        }

        BD_DEBUG(DBG_BLURAY, "Clip %s empty! (%p)\n", f_name, bd);
    }

    BD_DEBUG(DBG_BLURAY | DBG_CRIT, "Unable to open clip %s! (%p)\n",
          f_name, bd);

    X_FREE(f_name);
    return 0;
}

static int _read_block(BLURAY *bd, BD_STREAM *st, uint8_t *buf)
{
    const int len = 6144;

    if (st->fp) {
        BD_DEBUG(DBG_STREAM, "Reading unit [%d bytes] at %"PRIu64"... (%p)\n",
              len, st->clip_block_pos, bd);

        if (len + st->clip_block_pos <= st->clip_size) {
            int read_len;

            if ((read_len = file_read(st->fp, buf, len))) {
                if (read_len != len)
                    BD_DEBUG(DBG_STREAM | DBG_CRIT, "Read %d bytes at %"PRIu64" ; requested %d ! (%p)\n", read_len, st->clip_block_pos, len, bd);

                if (bd->aacs && bd->libaacs_decrypt_unit) {
                    if (!bd->libaacs_decrypt_unit(bd->aacs, buf)) {
                        BD_DEBUG(DBG_AACS | DBG_CRIT, "Unable decrypt unit (AACS)! (%p)\n", bd);

                        return 0;
                    } // decrypt
                } // aacs

                st->clip_block_pos += len;

                // bdplus fixup, if required.
                if (bd->bdplus_fixup && bd->bdplus) {
                    int32_t numFixes;
                    numFixes = bd->bdplus_fixup(bd->bdplus, len, buf);
#if 1
                    if (numFixes) {
                        BD_DEBUG(DBG_BDPLUS,
                              "BDPLUS did %u fixups\n", numFixes);
                    }
#endif

                }

                /* Check TP_extra_header Copy_permission_indicator. If != 0, unit is still encrypted. */
                if (buf[0] & 0xc0) {
                    BD_DEBUG(DBG_BLURAY | DBG_CRIT,
                          "TP header copy permission indicator != 0, unit is still encrypted? (%p)\n", bd);
                    _queue_event(bd, (BD_EVENT){BD_EVENT_ENCRYPTED, 0});
                    return 0;
                }

                BD_DEBUG(DBG_STREAM, "Read unit OK! (%p)\n", bd);

                return 1;
            }

            BD_DEBUG(DBG_STREAM | DBG_CRIT, "Read %d bytes at %"PRIu64" failed ! (%p)\n", len, st->clip_block_pos, bd);

            return 0;
        }

        BD_DEBUG(DBG_STREAM | DBG_CRIT, "Read past EOF ! (%p)\n", bd);

        return 0;
    }

    BD_DEBUG(DBG_BLURAY, "No valid title selected! (%p)\n", bd);

    return 0;
}

/*
 * clip preload (BD_PRELOAD)
 */

static void _close_preload(BD_PRELOAD *p)
{
    X_FREE(p->buf);
    memset(p, 0, sizeof(*p));
}

static int _preload_m2ts(BLURAY *bd, BD_PRELOAD *p)
{
    /* setup and open BD_STREAM */

    BD_STREAM st;

    memset(&st, 0, sizeof(st));
    st.clip = p->clip;

    if (!_open_m2ts(bd, &st)) {
        return 0;
    }

    /* allocate buffer */
    p->clip_size = st.clip_size;
    p->buf       = realloc(p->buf, p->clip_size);

    /* read clip to buffer */

    uint8_t *buf = p->buf;
    uint8_t *end = p->buf + p->clip_size;

    for (; buf < end; buf += 6144) {
        if (!_read_block(bd, &st, buf)) {
            BD_DEBUG(DBG_BLURAY|DBG_CRIT, "_preload_m2ts(): error loading %s at %"PRIu64"\n",
                  st.clip->name, (uint64_t)(buf - p->buf));
            _close_m2ts(&st);
            _close_preload(p);
            return 0;
        }
    }

    /* */

    BD_DEBUG(DBG_BLURAY, "_preload_m2ts(): loaded %"PRIu64" bytes from %s\n",
          st.clip_size, st.clip->name);

    _close_m2ts(&st);

    return 1;
}

static int64_t _seek_stream(BLURAY *bd, BD_STREAM *st,
                            NAV_CLIP *clip, uint32_t clip_pkt)
{
    if (!clip)
        return -1;

    if (!st->fp || !st->clip || clip->ref != st->clip->ref) {
        // The position is in a new clip
        st->clip = clip;
        if (!_open_m2ts(bd, st)) {
            return -1;
        }
    }

    st->clip_pos = (uint64_t)clip_pkt * 192;
    st->clip_block_pos = (st->clip_pos / 6144) * 6144;

    file_seek(st->fp, st->clip_block_pos, SEEK_SET);

    st->int_buf_off = 6144;

    return st->clip_pos;
}

/*
 * libaacs and libbdplus open / close
 */

static void _libaacs_close(BLURAY *bd)
{
    if (bd->aacs) {
        DL_CALL(bd->h_libaacs, aacs_close, bd->aacs);
        bd->aacs = NULL;
    }
}

static void _libaacs_unload(BLURAY *bd)
{
    _libaacs_close(bd);

#ifdef DLOPEN_CRYPTO_LIBS
    if (bd->h_libaacs) {
        dl_dlclose(bd->h_libaacs);
        bd->h_libaacs = NULL;
    }
#endif

    bd->libaacs_open         = NULL;
    bd->libaacs_decrypt_unit = NULL;
}

static int _libaacs_required(BLURAY *bd)
{
    BD_FILE_H *fd;
    char      *tmp;

    tmp = str_printf("%s/AACS/Unit_Key_RO.inf", bd->device_path);
    fd = file_open(tmp, "rb");
    X_FREE(tmp);

    if (fd) {
        file_close(fd);

        BD_DEBUG(DBG_BLURAY, "AACS/Unit_Key_RO.inf found. Disc seems to be AACS protected (%p)\n", bd);
        bd->disc_info.aacs_detected = 1;
        return 1;
    }

    BD_DEBUG(DBG_BLURAY, "AACS/Unit_Key_RO.inf not found. No AACS protection (%p)\n", bd);
    bd->disc_info.aacs_detected = 0;
    return 0;
}

static int _libaacs_load(BLURAY *bd)
{
#ifdef DLOPEN_CRYPTO_LIBS
    if (bd->h_libaacs) {
        return 1;
    }

    bd->disc_info.libaacs_detected = 0;
    if ((bd->h_libaacs = dl_dlopen("libaacs", "0"))) {

        BD_DEBUG(DBG_BLURAY, "Loading libaacs (%p)\n", bd->h_libaacs);

        bd->libaacs_open         = (fptr_p_void)dl_dlsym(bd->h_libaacs, "aacs_open");
        bd->libaacs_decrypt_unit = (fptr_int)dl_dlsym(bd->h_libaacs, "aacs_decrypt_unit");

        if (bd->libaacs_open && bd->libaacs_decrypt_unit) {
            BD_DEBUG(DBG_BLURAY, "Loaded libaacs (%p)\n", bd->h_libaacs);
            bd->disc_info.libaacs_detected = 1;
            return 1;

        } else {
            BD_DEBUG(DBG_BLURAY, "libaacs dlsym failed! (%p)\n", bd->h_libaacs);
        }

    } else {
        BD_DEBUG(DBG_BLURAY, "libaacs not found! (%p)\n", bd);
    }

    _libaacs_unload(bd);

    return 0;

#else
    BD_DEBUG(DBG_BLURAY, "Using libaacs via normal linking\n");

    bd->libaacs_open         = &aacs_open;
    bd->libaacs_decrypt_unit = &aacs_decrypt_unit;
    bd->disc_info.libaacs_detected = 1;

    return 1;
#endif
}

static int _libaacs_open(BLURAY *bd, const char *keyfile_path)
{
    _libaacs_close(bd);

    if (!_libaacs_required(bd)) {
        /* no AACS */
        return 1; /* no error if libaacs is not needed */
    }

    if (!_libaacs_load(bd)) {
        /* no libaacs */
        return 0;
    }

    bd->aacs = bd->libaacs_open(bd->device_path, keyfile_path);

    if (bd->aacs) {
        BD_DEBUG(DBG_BLURAY, "Opened libaacs (%p)\n", bd->aacs);
        bd->disc_info.aacs_handled = 1;
        return 1;
    }

    BD_DEBUG(DBG_BLURAY, "aacs_open() failed!\n");
    bd->disc_info.aacs_handled = 0;

    _libaacs_unload(bd);
    return 0;
}

static uint8_t *_libaacs_get_vid(BLURAY *bd)
{
    if (bd->aacs) {
#ifdef DLOPEN_CRYPTO_LIBS
        fptr_p_void fptr = (fptr_p_void)dl_dlsym(bd->h_libaacs, "aacs_get_vid");
        if (fptr) {
            return (uint8_t*)fptr(bd->aacs);
        }
        BD_DEBUG(DBG_BLURAY, "aacs_get_vid() dlsym failed! (%p)", bd);
        return NULL;
#else
        return aacs_get_vid(bd->aacs);
#endif
    }

    BD_DEBUG(DBG_BLURAY, "_libaacs_get_vid(): libaacs not initialized! (%p)", bd);
    return NULL;
}

static void _libbdplus_close(BLURAY *bd)
{
    if (bd->bdplus) {
        DL_CALL(bd->h_libbdplus, bdplus_free, bd->bdplus);
        bd->bdplus = NULL;
    }
}

static void _libbdplus_unload(BLURAY *bd)
{
    _libbdplus_close(bd);

#ifdef DLOPEN_CRYPTO_LIBS
    if (bd->h_libbdplus) {
        dl_dlclose(bd->h_libbdplus);
        bd->h_libbdplus = NULL;
    }
#endif

    bd->bdplus_init  = NULL;
    bd->bdplus_seek  = NULL;
    bd->bdplus_fixup = NULL;
}

static int _libbdplus_required(BLURAY *bd)
{
    BD_FILE_H *fd;
    char      *tmp;

    tmp = str_printf("%s/BDSVM/00000.svm", bd->device_path);
    fd = file_open(tmp, "rb");
    X_FREE(tmp);

    if (fd) {
        file_close(fd);

        BD_DEBUG(DBG_BLURAY, "BDSVM/00000.svm found. Disc seems to be BD+ protected (%p)\n", bd);
        bd->disc_info.bdplus_detected = 1;
        return 1;
    }

    BD_DEBUG(DBG_BLURAY, "BDSVM/00000.svm not found. No BD+ protection (%p)\n", bd);
    bd->disc_info.bdplus_detected = 0;
    return 0;
}

static int _libbdplus_load(BLURAY *bd)
{
    BD_DEBUG(DBG_BDPLUS, "attempting to load libbdplus\n");

#ifdef DLOPEN_CRYPTO_LIBS
    if (bd->h_libbdplus) {
        return 1;
    }

    bd->disc_info.libbdplus_detected = 0;
    if ((bd->h_libbdplus = dl_dlopen("libbdplus", "0"))) {

        BD_DEBUG(DBG_BLURAY, "Loading libbdplus (%p)\n", bd->h_libbdplus);

        bd->bdplus_init  = (fptr_p_void)dl_dlsym(bd->h_libbdplus, "bdplus_init");
        bd->bdplus_seek  = (fptr_int32)dl_dlsym(bd->h_libbdplus, "bdplus_seek");
        bd->bdplus_fixup = (fptr_int32)dl_dlsym(bd->h_libbdplus, "bdplus_fixup");

        if (bd->bdplus_init && bd->bdplus_seek && bd->bdplus_fixup) {
            BD_DEBUG(DBG_BLURAY, "Loaded libbdplus (%p)\n", bd->h_libbdplus);
            bd->disc_info.libbdplus_detected = 1;
            return 1;
        }

        BD_DEBUG(DBG_BLURAY, "libbdplus dlsym failed! (%p)\n", bd->h_libbdplus);

    } else {
        BD_DEBUG(DBG_BLURAY, "libbdplus not found! (%p)\n", bd);
    }

    _libbdplus_unload(bd);

    return 0;

#else
    BD_DEBUG(DBG_BLURAY,"Using libbdplus via normal linking\n");

    bd->bdplus_init  = &bdplus_init;
    bd->bdplus_seek  = &bdplus_seek;
    bd->bdplus_fixup = &bdplus_fixup;
    bd->disc_info.libbdplus_detected = 1;

    return 1;
#endif
}

static int _libbdplus_open(BLURAY *bd, const char *keyfile_path)
{
    // Take a quick stab to see if we want/need bdplus
    // we should fix this, and add various string functions.
    uint8_t vid[16] = {
        0xC5,0x43,0xEF,0x2A,0x15,0x0E,0x50,0xC4,0xE2,0xCA,
        0x71,0x65,0xB1,0x7C,0xA7,0xCB}; // FIXME

    _libbdplus_close(bd);

    if (!_libbdplus_required(bd)) {
        /* no BD+ */
        return 1; /* no error if libbdplus is not needed */
    }

    if (!_libbdplus_load(bd)) {
        /* no libbdplus */
        return 0;
    }

    const uint8_t *aacs_vid = (const uint8_t *)_libaacs_get_vid(bd);
    bd->bdplus = bd->bdplus_init(bd->device_path, keyfile_path, aacs_vid ? aacs_vid : vid);

    if (bd->bdplus) {
        BD_DEBUG(DBG_BLURAY,"libbdplus initialized\n");
        bd->disc_info.bdplus_handled = 1;
        return 1;
    }

    BD_DEBUG(DBG_BLURAY,"bdplus_init() failed\n");
    bd->disc_info.bdplus_handled = 0;

    _libbdplus_unload(bd);
    return 0;
}

/*
 * index open
 */

static int _index_open(BLURAY *bd)
{
    if (!bd->index) {
        char *file;

        file = str_printf("%s/BDMV/index.bdmv", bd->device_path);
        bd->index = indx_parse(file);
        X_FREE(file);
    }

    return !!bd->index;
}

/*
 * meta open
 */

static int _meta_open(BLURAY *bd)
{
    if (!bd->meta){
      bd->meta = meta_parse(bd->device_path);
    }

    return !!bd->meta;
}
/*
 * disc info
 */

const BLURAY_DISC_INFO *bd_get_disc_info(BLURAY *bd)
{
    return &bd->disc_info;
}

static void _fill_disc_info(BLURAY *bd)
{
    bd->disc_info.bluray_detected        = 0;
    bd->disc_info.top_menu_supported     = 0;
    bd->disc_info.first_play_supported   = 0;
    bd->disc_info.num_hdmv_titles        = 0;
    bd->disc_info.num_bdj_titles         = 0;
    bd->disc_info.num_unsupported_titles = 0;

    if (bd->index) {
        INDX_PLAY_ITEM *pi;
        unsigned        ii;

        bd->disc_info.bluray_detected = 1;

        pi = &bd->index->first_play;
        if (pi->object_type == indx_object_type_hdmv && pi->hdmv.id_ref != 0xffff) {
            bd->disc_info.first_play_supported = 1;
        }

        pi = &bd->index->top_menu;
        if (pi->object_type == indx_object_type_hdmv && pi->hdmv.id_ref != 0xffff) {
            bd->disc_info.top_menu_supported = 1;
        }

        for (ii = 0; ii < bd->index->num_titles; ii++) {
            if (bd->index->titles[ii].object_type == indx_object_type_hdmv) {
                bd->disc_info.num_hdmv_titles++;
            }
            if (bd->index->titles[ii].object_type == indx_object_type_bdj) {
                bd->disc_info.num_bdj_titles++;
                bd->disc_info.num_unsupported_titles++;
            }
        }
    }
}

/*
 * open / close
 */

BLURAY *bd_open(const char* device_path, const char* keyfile_path)
{
    BLURAY *bd = calloc(1, sizeof(BLURAY));

    if (device_path) {

        bd->device_path = (char*)malloc(strlen(device_path) + 1);
        strcpy(bd->device_path, device_path);

        _libaacs_open(bd, keyfile_path);

        _libbdplus_open(bd, keyfile_path);

        _index_open(bd);

        bd->meta = NULL;

        bd->regs = bd_registers_init();

        _fill_disc_info(bd);

        BD_DEBUG(DBG_BLURAY, "BLURAY initialized! (%p)\n", bd);
    } else {
        X_FREE(bd);

        BD_DEBUG(DBG_BLURAY | DBG_CRIT, "No device path provided!\n");
    }

    return bd;
}

void bd_close(BLURAY *bd)
{
    bd_stop_bdj(bd);

    _libaacs_unload(bd);

    _libbdplus_unload(bd);

    _close_m2ts(&bd->st0);
    _close_preload(&bd->st_ig);

    if (bd->title_list != NULL) {
        nav_free_title_list(bd->title_list);
    }
    if (bd->title != NULL) {
        nav_title_close(bd->title);
    }

    hdmv_vm_free(&bd->hdmv_vm);

    gc_free(&bd->graphics_controller);
    indx_free(&bd->index);
    bd_registers_free(bd->regs);

    X_FREE(bd->event_queue);
    X_FREE(bd->device_path);

    BD_DEBUG(DBG_BLURAY, "BLURAY destroyed! (%p)\n", bd);

    X_FREE(bd);
}

/*
 * seeking and current position
 */

static int64_t _seek_internal(BLURAY *bd,
                              NAV_CLIP *clip, uint32_t title_pkt, uint32_t clip_pkt)
{
    if (_seek_stream(bd, &bd->st0, clip, clip_pkt) >= 0) {

        /* update title position */
        bd->s_pos = (uint64_t)title_pkt * 192;

        /* chapter tracking */
        _update_chapter_psr(bd);

        BD_DEBUG(DBG_BLURAY, "Seek to %"PRIu64" (%p)\n",
              bd->s_pos, bd);

        if (bd->bdplus_seek && bd->bdplus) {
            bd->bdplus_seek(bd->bdplus, bd->st0.clip_block_pos);
        }
    }

    return bd->s_pos;
}

/* _change_angle() should be used only before call to _seek_internal() ! */
static void _change_angle(BLURAY *bd)
{
    if (bd->seamless_angle_change) {
        bd->st0.clip = nav_set_angle(bd->title, bd->st0.clip, bd->request_angle);
        bd->seamless_angle_change = 0;
        bd_psr_write(bd->regs, PSR_ANGLE_NUMBER, bd->title->angle + 1);

        /* force re-opening .m2ts file in _seek_internal() */
        _close_m2ts(&bd->st0);
    }
}

int64_t bd_seek_time(BLURAY *bd, uint64_t tick)
{
    uint32_t clip_pkt, out_pkt;
    NAV_CLIP *clip;

    tick /= 2;

    if (bd->title &&
        tick < bd->title->duration) {

        _change_angle(bd);

        // Find the closest access unit to the requested position
        clip = nav_time_search(bd->title, tick, &clip_pkt, &out_pkt);

        return _seek_internal(bd, clip, out_pkt, clip_pkt);
    }

    return bd->s_pos;
}

uint64_t bd_tell_time(BLURAY *bd)
{
    uint32_t clip_pkt = 0, out_pkt = 0, out_time = 0;

    if (bd && bd->title) {
        nav_packet_search(bd->title, bd->s_pos / 192, &clip_pkt, &out_pkt, &out_time);
    }

    return ((uint64_t)out_time) * 2;
}

int64_t bd_seek_chapter(BLURAY *bd, unsigned chapter)
{
    uint32_t clip_pkt, out_pkt;
    NAV_CLIP *clip;

    if (bd->title &&
        chapter < bd->title->chap_list.count) {

        _change_angle(bd);

        // Find the closest access unit to the requested position
        clip = nav_chapter_search(bd->title, chapter, &clip_pkt, &out_pkt);

        return _seek_internal(bd, clip, out_pkt, clip_pkt);
    }

    return bd->s_pos;
}

int64_t bd_chapter_pos(BLURAY *bd, unsigned chapter)
{
    uint32_t clip_pkt, out_pkt;

    if (bd->title &&
        chapter < bd->title->chap_list.count) {

        // Find the closest access unit to the requested position
        nav_chapter_search(bd->title, chapter, &clip_pkt, &out_pkt);
        return (int64_t)out_pkt * 192;
    }

    return -1;
}

uint32_t bd_get_current_chapter(BLURAY *bd)
{
    if (bd->title) {
        return nav_chapter_get_current(bd->st0.clip, bd->st0.clip_pos / 192);
    }

    return 0;
}

int64_t bd_seek_playitem(BLURAY *bd, unsigned clip_ref)
{
    uint32_t clip_pkt, out_pkt;
    NAV_CLIP *clip;

    if (bd->title &&
        clip_ref < bd->title->clip_list.count) {

      _change_angle(bd);

      clip     = &bd->title->clip_list.clip[clip_ref];
      clip_pkt = clip->start_pkt;
      out_pkt  = clip->pos;

      return _seek_internal(bd, clip, out_pkt, clip_pkt);
    }

    return bd->s_pos;
}

int64_t bd_seek_mark(BLURAY *bd, unsigned mark)
{
    uint32_t clip_pkt, out_pkt;
    NAV_CLIP *clip;

    if (bd->title &&
        mark < bd->title->mark_list.count) {

        _change_angle(bd);

        // Find the closest access unit to the requested position
        clip = nav_mark_search(bd->title, mark, &clip_pkt, &out_pkt);

        return _seek_internal(bd, clip, out_pkt, clip_pkt);
    }

    return bd->s_pos;
}

int64_t bd_seek(BLURAY *bd, uint64_t pos)
{
    uint32_t pkt, clip_pkt, out_pkt, out_time;
    NAV_CLIP *clip;

    if (bd->title &&
        pos < (uint64_t)bd->title->packets * 192) {

        pkt = pos / 192;

        _change_angle(bd);

        // Find the closest access unit to the requested position
        clip = nav_packet_search(bd->title, pkt, &clip_pkt, &out_pkt, &out_time);

        return _seek_internal(bd, clip, out_pkt, clip_pkt);
    }

    return bd->s_pos;
}

uint64_t bd_get_title_size(BLURAY *bd)
{
    if (bd && bd->title) {
        return (uint64_t)bd->title->packets * 192;
    }
    return UINT64_C(0);
}

uint64_t bd_tell(BLURAY *bd)
{
    return bd ? bd->s_pos : INT64_C(0);
}

/*
 * read
 */

static int64_t _clip_seek_time(BLURAY *bd, uint64_t tick)
{
    uint32_t clip_pkt, out_pkt;

    if (tick < bd->st0.clip->out_time) {

        // Find the closest access unit to the requested position
        nav_clip_time_search(bd->st0.clip, tick, &clip_pkt, &out_pkt);

        return _seek_internal(bd, bd->st0.clip, out_pkt, clip_pkt);
    }

    return bd->s_pos;
}

int bd_read(BLURAY *bd, unsigned char *buf, int len)
{
    BD_STREAM *st = &bd->st0;
    int out_len;

    if (st->fp) {
        out_len = 0;
        BD_DEBUG(DBG_STREAM, "Reading [%d bytes] at %"PRIu64"... (%p)\n", len, bd->s_pos, bd);

        while (len > 0) {
            uint32_t clip_pkt;

            unsigned int size = len;
            // Do we need to read more data?
            clip_pkt = st->clip_pos / 192;
            if (bd->seamless_angle_change) {
                if (clip_pkt >= bd->angle_change_pkt) {
                    if (clip_pkt >= st->clip->end_pkt) {
                        st->clip = nav_next_clip(bd->title, st->clip);
                        if (!_open_m2ts(bd, st)) {
                            return -1;
                        }
                        bd->s_pos = st->clip->pos;
                    } else {
                        _change_angle(bd);
                        _clip_seek_time(bd, bd->angle_change_time);
                    }
                    bd->seamless_angle_change = 0;
                } else {
                    uint64_t angle_pos;

                    angle_pos = bd->angle_change_pkt * 192;
                    if (angle_pos - st->clip_pos < size)
                    {
                        size = angle_pos - st->clip_pos;
                    }
                }
            }
            if (st->int_buf_off == 6144 || clip_pkt >= st->clip->end_pkt) {

                // Do we need to get the next clip?
                if (st->clip == NULL) {
                    // We previously reached the last clip.  Nothing
                    // else to read.
                    _queue_event(bd, (BD_EVENT){BD_EVENT_END_OF_TITLE, 0});
                    return 0;
                }
                if (clip_pkt >= st->clip->end_pkt) {

                    // split read()'s at clip boundary
                    if (out_len) {
                        return out_len;
                    }

                    MPLS_PI *pi = &st->clip->title->pl->play_item[st->clip->ref];

                    // handle still mode clips
                    if (pi->still_mode == BLURAY_STILL_INFINITE) {
                        _queue_event(bd, (BD_EVENT){BD_EVENT_STILL_TIME, 0});
                        return 0;
                    }
                    if (pi->still_mode == BLURAY_STILL_TIME) {
                        if (bd->event_queue) {
                            _queue_event(bd, (BD_EVENT){BD_EVENT_STILL_TIME, pi->still_time});
                            return 0;
                        }
                    }

                    // find next clip
                    st->clip = nav_next_clip(bd->title, st->clip);
                    if (st->clip == NULL) {
                        BD_DEBUG(DBG_BLURAY|DBG_STREAM, "End of title (%p)\n", bd);
                        _queue_event(bd, (BD_EVENT){BD_EVENT_END_OF_TITLE, 0});
                        return 0;
                    }
                    if (!_open_m2ts(bd, st)) {
                        return -1;
                    }
                }

                if (_read_block(bd, st, bd->int_buf)) {

                    st->int_buf_off = st->clip_pos % 6144;

                } else {
                    return out_len;
                }
            }
            if (size > (unsigned int)6144 - st->int_buf_off) {
                size = 6144 - st->int_buf_off;
            }
            memcpy(buf, bd->int_buf + st->int_buf_off, size);
            buf += size;
            len -= size;
            out_len += size;
            st->clip_pos += size;
            st->int_buf_off += size;
            bd->s_pos += size;
        }

        /* chapter tracking */
        if (bd->s_pos > bd->next_chapter_start) {
            _update_chapter_psr(bd);
        }

        BD_DEBUG(DBG_STREAM, "%d bytes read OK! (%p)\n", out_len, bd);

        return out_len;
    }

    BD_DEBUG(DBG_STREAM | DBG_CRIT, "bd_read(): no valid title selected! (%p)\n", bd);

    return -1;
}

int bd_read_skip_still(BLURAY *bd)
{
    BD_STREAM *st = &bd->st0;

    if (st->clip) {
        MPLS_PI *pi = &st->clip->title->pl->play_item[st->clip->ref];

        if (pi->still_mode == BLURAY_STILL_TIME) {
            st->clip = nav_next_clip(bd->title, st->clip);
            if (st->clip) {
                return _open_m2ts(bd, st);
            }
        }
    }

    return 0;
}

/*
 * preloader for asynchronous sub paths
 */

static int _find_ig_stream(BLURAY *bd, uint16_t *pid, int *sub_path_idx)
{
    MPLS_PI  *pi        = &bd->title->pl->play_item[0];
    unsigned  ig_stream = bd_psr_read(bd->regs, PSR_IG_STREAM_ID);

    if (ig_stream > 0 && ig_stream <= pi->stn.num_ig) {
        ig_stream--;
        if (pi->stn.ig[ig_stream].stream_type == 2) {
            *sub_path_idx = pi->stn.ig[ig_stream].subpath_id;
        }
        *pid = pi->stn.ig[ig_stream].pid;

        BD_DEBUG(DBG_BLURAY, "_find_ig_stream(): current IG stream pid 0x%04x sub-path %d\n",
              *pid, *sub_path_idx);
        return 1;
    }

    return 0;
}

static int _preload_ig_subpath(BLURAY *bd)
{
    int      ig_subpath = -1;
    uint16_t ig_pid     = 0;

    _find_ig_stream(bd, &ig_pid, &ig_subpath);

    if (!bd->graphics_controller) {
        return 0;
    }

    if (ig_subpath < 0) {
        return 0;
    }

    bd->st_ig.clip = &bd->title->sub_path[ig_subpath].clip_list.clip[0];

    if (!_preload_m2ts(bd, &bd->st_ig)) {
        return 0;
    }

    gc_decode_ts(bd->graphics_controller, ig_pid, bd->st_ig.buf, bd->st_ig.clip_size / 6144, -1);

    return 1;
}

static int _preload_subpaths(BLURAY *bd)
{
    if (bd->title->pl->sub_count <= 0) {
        return 0;
    }

    return _preload_ig_subpath(bd);
}

/*
 * select title / angle
 */

static void _close_playlist(BLURAY *bd)
{
    if (bd->graphics_controller) {
        gc_run(bd->graphics_controller, GC_CTRL_RESET, 0, NULL);
    }

    _close_m2ts(&bd->st0);
    _close_preload(&bd->st_ig);

    if (bd->title) {
        nav_title_close(bd->title);
        bd->title = NULL;
    }
}

static int _open_playlist(BLURAY *bd, const char *f_name, unsigned angle)
{
    _close_playlist(bd);

    bd->title = nav_title_open(bd->device_path, f_name, angle);
    if (bd->title == NULL) {
        BD_DEBUG(DBG_BLURAY | DBG_CRIT, "Unable to open title %s! (%p)\n",
              f_name, bd);
        return 0;
    }

    bd->seamless_angle_change = 0;
    bd->s_pos = 0;

    bd->next_chapter_start = bd_chapter_pos(bd, 1);

    bd_psr_write(bd->regs, PSR_PLAYLIST, atoi(bd->title->name));
    bd_psr_write(bd->regs, PSR_ANGLE_NUMBER, bd->title->angle + 1);
    bd_psr_write(bd->regs, PSR_CHAPTER, 1);

    // Get the initial clip of the playlist
    bd->st0.clip = nav_next_clip(bd->title, NULL);
    if (_open_m2ts(bd, &bd->st0)) {
        BD_DEBUG(DBG_BLURAY, "Title %s selected! (%p)\n", f_name, bd);

        _preload_subpaths(bd);

        return 1;
    }
    return 0;
}

int bd_select_playlist(BLURAY *bd, uint32_t playlist)
{
    char *f_name = str_printf("%05d.mpls", playlist);
    int result;

    if (bd->title_list) {
        /* update current title */
        unsigned i;
        for (i = 0; i < bd->title_list->count; i++) {
            if (playlist == bd->title_list->title_info[i].mpls_id) {
                bd->title_idx = i;
                break;
            }
        }
    }

    result = _open_playlist(bd, f_name, 0);

    X_FREE(f_name);
    return result;
}

// Select a title for playback
// The title index is an index into the list
// established by bd_get_titles()
int bd_select_title(BLURAY *bd, uint32_t title_idx)
{
    const char *f_name;

    // Open the playlist
    if (bd->title_list == NULL) {
        BD_DEBUG(DBG_BLURAY, "Title list not yet read! (%p)\n", bd);
        return 0;
    }
    if (bd->title_list->count <= title_idx) {
        BD_DEBUG(DBG_BLURAY, "Invalid title index %d! (%p)\n", title_idx, bd);
        return 0;
    }

    bd->title_idx = title_idx;
    f_name = bd->title_list->title_info[title_idx].name;

    return _open_playlist(bd, f_name, 0);
}

uint32_t bd_get_current_title(BLURAY *bd)
{
    return bd->title_idx;
}

int bd_select_angle(BLURAY *bd, unsigned angle)
{
    unsigned orig_angle;

    if (bd->title == NULL) {
        BD_DEBUG(DBG_BLURAY, "Title not yet selected! (%p)\n", bd);
        return 0;
    }

    orig_angle = bd->title->angle;

    bd->st0.clip = nav_set_angle(bd->title, bd->st0.clip, angle);

    if (orig_angle == bd->title->angle) {
        return 1;
    }

    bd_psr_write(bd->regs, PSR_ANGLE_NUMBER, bd->title->angle + 1);

    if (!_open_m2ts(bd, &bd->st0)) {
        BD_DEBUG(DBG_BLURAY|DBG_CRIT, "Error selecting angle %d ! (%p)\n", angle, bd);
        return 0;
    }

    return 1;
}

unsigned bd_get_current_angle(BLURAY *bd)
{
    if (bd->title) {
        return bd->title->angle;
    }
    return 0;
}


void bd_seamless_angle_change(BLURAY *bd, unsigned angle)
{
    uint32_t clip_pkt;

    clip_pkt = (bd->st0.clip_pos + 191) / 192;
    bd->angle_change_pkt = nav_angle_change_search(bd->st0.clip, clip_pkt,
                                                   &bd->angle_change_time);
    bd->request_angle = angle;
    bd->seamless_angle_change = 1;
}

/*
 * title lists
 */

uint32_t bd_get_titles(BLURAY *bd, uint8_t flags, uint32_t min_title_length)
{
    if (!bd) {
        BD_DEBUG(DBG_BLURAY | DBG_CRIT, "bd_get_titles(NULL) failed (%p)\n", bd);
        return 0;
    }

    if (bd->title_list != NULL) {
        nav_free_title_list(bd->title_list);
    }
    bd->title_list = nav_get_title_list(bd->device_path, flags, min_title_length);

    if (!bd->title_list) {
        BD_DEBUG(DBG_BLURAY | DBG_CRIT, "nav_get_title_list(%s) failed (%p)\n", bd->device_path, bd);
        return 0;
    }

    return bd->title_list->count;
}

static void _copy_streams(NAV_CLIP *clip, BLURAY_STREAM_INFO *streams, MPLS_STREAM *si, int count)
{
    int ii;

    for (ii = 0; ii < count; ii++) {
        streams[ii].coding_type = si[ii].coding_type;
        streams[ii].format = si[ii].format;
        streams[ii].rate = si[ii].rate;
        streams[ii].char_code = si[ii].char_code;
        memcpy(streams[ii].lang, si[ii].lang, 4);
        streams[ii].pid = si[ii].pid;
        streams[ii].aspect = nav_lookup_aspect(clip, si[ii].pid);
    }
}

static BLURAY_TITLE_INFO* _fill_title_info(NAV_TITLE* title, uint32_t title_idx, uint32_t playlist)
{
    BLURAY_TITLE_INFO *title_info;
    unsigned int ii;

    title_info = calloc(1, sizeof(BLURAY_TITLE_INFO));
    title_info->idx = title_idx;
    title_info->playlist = playlist;
    title_info->duration = (uint64_t)title->duration * 2;
    title_info->angle_count = title->angle_count;
    title_info->chapter_count = title->chap_list.count;
    title_info->chapters = calloc(title_info->chapter_count, sizeof(BLURAY_TITLE_CHAPTER));
    for (ii = 0; ii < title_info->chapter_count; ii++) {
        title_info->chapters[ii].idx = ii;
        title_info->chapters[ii].start = (uint64_t)title->chap_list.mark[ii].title_time * 2;
        title_info->chapters[ii].duration = (uint64_t)title->chap_list.mark[ii].duration * 2;
        title_info->chapters[ii].offset = (uint64_t)title->chap_list.mark[ii].title_pkt * 192L;
    }
    title_info->clip_count = title->clip_list.count;
    title_info->clips = calloc(title_info->clip_count, sizeof(BLURAY_CLIP_INFO));
    for (ii = 0; ii < title_info->clip_count; ii++) {
        MPLS_PI *pi = &title->pl->play_item[ii];
        BLURAY_CLIP_INFO *ci = &title_info->clips[ii];
        NAV_CLIP *nc = &title->clip_list.clip[ii];

        ci->pkt_count = nc->end_pkt - nc->start_pkt;
        ci->still_mode = pi->still_mode;
        ci->still_time = pi->still_time;
        ci->video_stream_count = pi->stn.num_video;
        ci->audio_stream_count = pi->stn.num_audio;
        ci->pg_stream_count = pi->stn.num_pg + pi->stn.num_pip_pg;
        ci->ig_stream_count = pi->stn.num_ig;
        ci->sec_video_stream_count = pi->stn.num_secondary_video;
        ci->sec_audio_stream_count = pi->stn.num_secondary_audio;
        ci->video_streams = calloc(ci->video_stream_count, sizeof(BLURAY_STREAM_INFO));
        ci->audio_streams = calloc(ci->audio_stream_count, sizeof(BLURAY_STREAM_INFO));
        ci->pg_streams = calloc(ci->pg_stream_count, sizeof(BLURAY_STREAM_INFO));
        ci->ig_streams = calloc(ci->ig_stream_count, sizeof(BLURAY_STREAM_INFO));
        ci->sec_video_streams = calloc(ci->sec_video_stream_count, sizeof(BLURAY_STREAM_INFO));
        ci->sec_audio_streams = calloc(ci->sec_audio_stream_count, sizeof(BLURAY_STREAM_INFO));
        _copy_streams(nc, ci->video_streams, pi->stn.video, ci->video_stream_count);
        _copy_streams(nc, ci->audio_streams, pi->stn.audio, ci->audio_stream_count);
        _copy_streams(nc, ci->pg_streams, pi->stn.pg, ci->pg_stream_count);
        _copy_streams(nc, ci->ig_streams, pi->stn.ig, ci->ig_stream_count);
        _copy_streams(nc, ci->sec_video_streams, pi->stn.secondary_video, ci->sec_video_stream_count);
        _copy_streams(nc, ci->sec_audio_streams, pi->stn.secondary_audio, ci->sec_audio_stream_count);
    }

    return title_info;
}

static BLURAY_TITLE_INFO *_get_title_info(BLURAY *bd, uint32_t title_idx, uint32_t playlist, const char *mpls_name,
                                          unsigned angle)
{
    NAV_TITLE *title;
    BLURAY_TITLE_INFO *title_info;

    title = nav_title_open(bd->device_path, mpls_name, angle);
    if (title == NULL) {
        BD_DEBUG(DBG_BLURAY | DBG_CRIT, "Unable to open title %s! (%p)\n",
              mpls_name, bd);
        return NULL;
    }

    title_info = _fill_title_info(title, title_idx, playlist);

    nav_title_close(title);
    return title_info;
}

BLURAY_TITLE_INFO* bd_get_title_info(BLURAY *bd, uint32_t title_idx, unsigned angle)
{
    if (bd->title_list == NULL) {
        BD_DEBUG(DBG_BLURAY, "Title list not yet read! (%p)\n", bd);
        return NULL;
    }
    if (bd->title_list->count <= title_idx) {
        BD_DEBUG(DBG_BLURAY, "Invalid title index %d! (%p)\n", title_idx, bd);
        return NULL;
    }

    return _get_title_info(bd,
                           title_idx, bd->title_list->title_info[title_idx].mpls_id,
                           bd->title_list->title_info[title_idx].name,
                           angle);
}

BLURAY_TITLE_INFO* bd_get_playlist_info(BLURAY *bd, uint32_t playlist, unsigned angle)
{
    char *f_name = str_printf("%05d.mpls", playlist);
    BLURAY_TITLE_INFO *title_info;

    title_info = _get_title_info(bd, 0, playlist, f_name, angle);

    X_FREE(f_name);

    return title_info;
}

void bd_free_title_info(BLURAY_TITLE_INFO *title_info)
{
    unsigned int ii;

    X_FREE(title_info->chapters);
    for (ii = 0; ii < title_info->clip_count; ii++) {
        X_FREE(title_info->clips[ii].video_streams);
        X_FREE(title_info->clips[ii].audio_streams);
        X_FREE(title_info->clips[ii].pg_streams);
        X_FREE(title_info->clips[ii].ig_streams);
        X_FREE(title_info->clips[ii].sec_video_streams);
        X_FREE(title_info->clips[ii].sec_audio_streams);
    }
    X_FREE(title_info->clips);
    X_FREE(title_info);
}

/*
 * player settings
 */

int bd_set_player_setting(BLURAY *bd, uint32_t idx, uint32_t value)
{
    static const struct { uint32_t idx; uint32_t  psr; } map[] = {
        { BLURAY_PLAYER_SETTING_PARENTAL,       PSR_PARENTAL },
        { BLURAY_PLAYER_SETTING_AUDIO_CAP,      PSR_AUDIO_CAP },
        { BLURAY_PLAYER_SETTING_AUDIO_LANG,     PSR_AUDIO_LANG },
        { BLURAY_PLAYER_SETTING_PG_LANG,        PSR_PG_AND_SUB_LANG },
        { BLURAY_PLAYER_SETTING_MENU_LANG,      PSR_MENU_LANG },
        { BLURAY_PLAYER_SETTING_COUNTRY_CODE,   PSR_COUNTRY },
        { BLURAY_PLAYER_SETTING_REGION_CODE,    PSR_REGION },
        { BLURAY_PLAYER_SETTING_VIDEO_CAP,      PSR_VIDEO_CAP },
        { BLURAY_PLAYER_SETTING_TEXT_CAP,       PSR_TEXT_CAP },
        { BLURAY_PLAYER_SETTING_PLAYER_PROFILE, PSR_PROFILE_VERSION },
    };

    unsigned i;

    if (idx == BLURAY_PLAYER_SETTING_PLAYER_PROFILE) {
        value = ((value & 0xf) << 16) | 0x0200;  /* version fixed to BD-RO Part 3, version 2.0 */
    }

    for (i = 0; i < sizeof(map) / sizeof(map[0]); i++) {
        if (idx == map[i].idx) {
            return !bd_psr_setting_write(bd->regs, idx, value);
        }
    }

    return 0;
}

int bd_set_player_setting_str(BLURAY *bd, uint32_t idx, const char *s)
{
    switch (idx) {
        case BLURAY_PLAYER_SETTING_AUDIO_LANG:
        case BLURAY_PLAYER_SETTING_PG_LANG:
        case BLURAY_PLAYER_SETTING_MENU_LANG:
            return bd_set_player_setting(bd, idx, str_to_uint32(s, 3));

        case BLURAY_PLAYER_SETTING_COUNTRY_CODE:
            return bd_set_player_setting(bd, idx, str_to_uint32(s, 2));

        default:
            return 0;
    }
}

/*
 * bdj
 */

int bd_start_bdj(BLURAY *bd, const char *start_object)
{
#ifdef USING_BDJAVA
    if (bd->bdjava == NULL) {
        bd->bdjava = bdj_open(bd->device_path, start_object, bd, bd->regs);
        return !!bd->bdjava;
    } else {
        BD_DEBUG(DBG_BLURAY | DBG_CRIT, "BD-J is already running (%p)\n", bd);
        return 1;
    }
#else
    BD_DEBUG(DBG_BLURAY | DBG_CRIT, "%s.bdjo: BD-J not compiled in (%p)\n", start_object, bd);
#endif
    return 0;
}

void bd_stop_bdj(BLURAY *bd)
{
    if (bd->bdjava != NULL) {
#ifdef USING_BDJAVA
        bdj_close((BDJAVA*)bd->bdjava);
#else
        BD_DEBUG(DBG_BLURAY, "BD-J not compiled in (%p)\n", bd);
#endif
        bd->bdjava = NULL;
    }
}

/*
 * Navigation mode interface
 */

static void _process_psr_restore_event(BLURAY *bd, BD_PSR_EVENT *ev)
{
    /* PSR restore events are handled internally.
     * Restore stored playback position.
     */

    BD_DEBUG(DBG_BLURAY, "PSR restore: psr%u = %u (%p)\n", ev->psr_idx, ev->new_val, bd);

    switch (ev->psr_idx) {
        case PSR_ANGLE_NUMBER:
            /* can't set angle before playlist is opened */
            return;
        case PSR_TITLE_NUMBER:
            /* pass to the application */
            _queue_event(bd, (BD_EVENT){BD_EVENT_TITLE, ev->new_val});
            return;
        case PSR_CHAPTER:
            /* will be selected automatically */
            return;
        case PSR_PLAYLIST:
            bd_select_playlist(bd, ev->new_val);
            nav_set_angle(bd->title, bd->st0.clip, bd_psr_read(bd->regs, PSR_ANGLE_NUMBER) - 1);
            return;
        case PSR_PLAYITEM:
            bd_seek_playitem(bd, ev->new_val);
            return;
        case PSR_TIME:
            bd_seek_time(bd, ((int64_t)ev->new_val) << 1);
            return;

        case PSR_SELECTED_BUTTON_ID:
        case PSR_MENU_PAGE_ID:
            /* handled by graphics controller */
            return;

        default:
            /* others: ignore */
            return;
    }
}

/*
 * notification events to APP
 */

static void _process_psr_write_event(BLURAY *bd, BD_PSR_EVENT *ev)
{
    if (ev->ev_type == BD_PSR_WRITE) {
        BD_DEBUG(DBG_BLURAY, "PSR write: psr%u = %u (%p)\n", ev->psr_idx, ev->new_val, bd);
    }

    switch (ev->psr_idx) {

        /* current playback position */

        case PSR_ANGLE_NUMBER: _queue_event(bd, (BD_EVENT){BD_EVENT_ANGLE,    ev->new_val}); break;
        case PSR_TITLE_NUMBER: _queue_event(bd, (BD_EVENT){BD_EVENT_TITLE,    ev->new_val}); break;
        case PSR_PLAYLIST:     _queue_event(bd, (BD_EVENT){BD_EVENT_PLAYLIST, ev->new_val}); break;
        case PSR_PLAYITEM:     _queue_event(bd, (BD_EVENT){BD_EVENT_PLAYITEM, ev->new_val}); break;
        case PSR_CHAPTER:      _queue_event(bd, (BD_EVENT){BD_EVENT_CHAPTER,  ev->new_val}); break;

        default:;
    }
}

static void _process_psr_change_event(BLURAY *bd, BD_PSR_EVENT *ev)
{
    BD_DEBUG(DBG_BLURAY, "PSR change: psr%u = %u (%p)\n", ev->psr_idx, ev->new_val, bd);

    _process_psr_write_event(bd, ev);

    switch (ev->psr_idx) {

        /* stream selection */

        case PSR_IG_STREAM_ID:
            _queue_event(bd, (BD_EVENT){BD_EVENT_IG_STREAM, ev->new_val});
            break;

        case PSR_PRIMARY_AUDIO_ID:
            _queue_event(bd, (BD_EVENT){BD_EVENT_AUDIO_STREAM, ev->new_val});
            break;

        case PSR_PG_STREAM:
            if ((ev->new_val & 0x80000fff) != (ev->old_val & 0x80000fff)) {
                _queue_event(bd, (BD_EVENT){BD_EVENT_PG_TEXTST,        !!(ev->new_val & 0x80000000)});
                _queue_event(bd, (BD_EVENT){BD_EVENT_PG_TEXTST_STREAM,    ev->new_val & 0xfff});
            }
            break;

        case PSR_SECONDARY_AUDIO_VIDEO:
            /* secondary video */
            if ((ev->new_val & 0x8f00ff00) != (ev->old_val & 0x8f00ff00)) {
                _queue_event(bd, (BD_EVENT){BD_EVENT_SECONDARY_VIDEO, !!(ev->new_val & 0x80000000)});
                _queue_event(bd, (BD_EVENT){BD_EVENT_SECONDARY_VIDEO_SIZE, (ev->new_val >> 24) & 0xf});
                _queue_event(bd, (BD_EVENT){BD_EVENT_SECONDARY_VIDEO_STREAM, (ev->new_val & 0xff00) >> 8});
            }
            /* secondary audio */
            if ((ev->new_val & 0x400000ff) != (ev->old_val & 0x400000ff)) {
                _queue_event(bd, (BD_EVENT){BD_EVENT_SECONDARY_AUDIO, !!(ev->new_val & 0x40000000)});
                _queue_event(bd, (BD_EVENT){BD_EVENT_SECONDARY_AUDIO_STREAM, ev->new_val & 0xff});
            }
            break;

        default:;
    }
}

static void _process_psr_event(void *handle, BD_PSR_EVENT *ev)
{
    BLURAY *bd = (BLURAY*)handle;

    switch(ev->ev_type) {
        case BD_PSR_WRITE:
            _process_psr_write_event(bd, ev);
            break;
        case BD_PSR_CHANGE:
            _process_psr_change_event(bd, ev);
            break;
        case BD_PSR_RESTORE:
            _process_psr_restore_event(bd, ev);
            break;

        case BD_PSR_SAVE:
            BD_DEBUG(DBG_BLURAY, "PSR save event (%p)\n", bd);
            break;
        default:
            BD_DEBUG(DBG_BLURAY, "PSR event %d: psr%u = %u (%p)\n", ev->ev_type, ev->psr_idx, ev->new_val, bd);
            break;
    }
}

static void _queue_initial_psr_events(BLURAY *bd)
{
    const uint32_t psrs[] = {
        PSR_ANGLE_NUMBER,
        PSR_TITLE_NUMBER,
        PSR_CHAPTER,
        PSR_PLAYLIST,
        PSR_PLAYITEM,
        PSR_IG_STREAM_ID,
        PSR_PRIMARY_AUDIO_ID,
        PSR_PG_STREAM,
        PSR_SECONDARY_AUDIO_VIDEO,
    };
    unsigned ii;

    for (ii = 0; ii < sizeof(psrs) / sizeof(psrs[0]); ii++) {
        BD_PSR_EVENT ev = {
            .ev_type = BD_PSR_CHANGE,
            .psr_idx = psrs[ii],
            .old_val = 0,
            .new_val = bd_psr_read(bd->regs, psrs[ii]),
        };

        _process_psr_change_event(bd, &ev);
    }
}

static int _play_bdj(BLURAY *bd, const char *name)
{
    bd->title_type = title_bdj;

#ifdef USING_BDJAVA
    bd_stop_bdj(bd);
    return bd_start_bdj(bd, name);
#else
    BD_DEBUG(DBG_BLURAY|DBG_CRIT, "_bdj_play(BDMV/BDJ/%s.jar) not implemented (%p)\n", name, bd);
    return 0;
#endif
}

static int _play_hdmv(BLURAY *bd, unsigned id_ref)
{
    int result = 1;

    bd->title_type = title_hdmv;

#ifdef USING_BDJAVA
    bd_stop_bdj(bd);
#endif

    if (!bd->hdmv_vm) {
        bd->hdmv_vm = hdmv_vm_init(bd->device_path, bd->regs, bd->index);
    }

    if (hdmv_vm_select_object(bd->hdmv_vm, id_ref)) {
        result = 0;
    }

    bd->hdmv_suspended = !hdmv_vm_running(bd->hdmv_vm);

    return result;
}

static int _play_title(BLURAY *bd, unsigned title)
{
    /* first play object ? */
    if (title == BLURAY_TITLE_FIRST_PLAY) {
        INDX_PLAY_ITEM *p = &bd->index->first_play;

        bd_psr_write(bd->regs, PSR_TITLE_NUMBER, 0xffff); /* 5.2.3.3 */

        if (p->object_type == indx_object_type_hdmv) {
            if (p->hdmv.id_ref == 0xffff) {
                /* no first play title (5.2.3.3) */
                bd->title_type = title_hdmv;
                return 1;
            }
            return _play_hdmv(bd, p->hdmv.id_ref);
        }

        if (p->object_type == indx_object_type_bdj) {
            return _play_bdj(bd, p->bdj.name);
        }

        return 0;
    }

    /* bd_play not called ? */
    if (bd->title_type == title_undef) {
        BD_DEBUG(DBG_BLURAY|DBG_CRIT, "bd_call_title(): bd_play() not called !\n");
        return 0;
    }

    /* top menu ? */
    if (title == BLURAY_TITLE_TOP_MENU) {
        INDX_PLAY_ITEM *p = &bd->index->top_menu;

        bd_psr_write(bd->regs, PSR_TITLE_NUMBER, 0); /* 5.2.3.3 */

        if (p->object_type == indx_object_type_hdmv) {
            if (p->hdmv.id_ref == 0xffff) {
                /* no top menu (5.2.3.3) */
                bd->title_type = title_hdmv;
                return 0;
            }
            return _play_hdmv(bd, p->hdmv.id_ref);
        }

        if (p->object_type == indx_object_type_bdj) {
            return _play_bdj(bd, p->bdj.name);
        }

        return 0;
    }

    /* valid title from disc index ? */
    if (title > 0 && title <= bd->index->num_titles) {
        INDX_TITLE *t = &bd->index->titles[title-1];

        bd_psr_write(bd->regs, PSR_TITLE_NUMBER, title); /* 5.2.3.3 */

        if (t->object_type == indx_object_type_hdmv) {
            return _play_hdmv(bd, t->hdmv.id_ref);
        } else {
            return _play_bdj(bd, t->bdj.name);
        }
    }

    return 0;
}

int bd_play(BLURAY *bd)
{
    /* reset player state */

    bd->title_type = title_undef;

    if (bd->hdmv_vm) {
        hdmv_vm_free(&bd->hdmv_vm);
    }

    _init_event_queue(bd);

    bd_psr_lock(bd->regs);
    bd_psr_register_cb(bd->regs, _process_psr_event, bd);
    _queue_initial_psr_events(bd);
    bd_psr_unlock(bd->regs);

    return _play_title(bd, BLURAY_TITLE_FIRST_PLAY);
}

int bd_play_title(BLURAY *bd, unsigned title)
{
    if (bd->title_type == title_undef && title != BLURAY_TITLE_FIRST_PLAY) {
        BD_DEBUG(DBG_BLURAY|DBG_CRIT, "bd_play_title(): bd_play() not called\n");
        return 0;
    }

    if (bd->st0.uo_mask.title_search) {
        BD_DEBUG(DBG_BLURAY|DBG_CRIT, "title search masked by stream\n");
        return 0;
    }

    if (bd->title_type == title_hdmv) {
        if (hdmv_vm_get_uo_mask(bd->hdmv_vm) & HDMV_TITLE_SEARCH_MASK) {
            BD_DEBUG(DBG_BLURAY|DBG_CRIT, "title search masked by movie object\n");
            return 0;
        }
    }

    return _play_title(bd, title);
}

int bd_menu_call(BLURAY *bd, int64_t pts)
{
    if (pts >= 0) {
        bd_psr_write(bd->regs, PSR_TIME, (uint32_t)(((uint64_t)pts) >> 1));
    }

    if (bd->title_type == title_undef) {
        BD_DEBUG(DBG_BLURAY|DBG_CRIT, "bd_menu_call(): bd_play() not called\n");
        return 0;
    }

    if (bd->st0.uo_mask.menu_call) {
        BD_DEBUG(DBG_BLURAY|DBG_CRIT, "menu call masked by stream\n");
        return 0;
    }

    if (bd->title_type == title_hdmv) {
        if (hdmv_vm_get_uo_mask(bd->hdmv_vm) & HDMV_MENU_CALL_MASK) {
            BD_DEBUG(DBG_BLURAY|DBG_CRIT, "menu call masked by movie object\n");
            return 0;
        }

        if (hdmv_vm_suspend_pl(bd->hdmv_vm) < 0) {
            BD_DEBUG(DBG_BLURAY|DBG_CRIT, "bd_menu_call(): error storing playback location\n");
        }
    }

    return _play_title(bd, BLURAY_TITLE_TOP_MENU);
}

static int _run_gc(BLURAY *bd, gc_ctrl_e msg, uint32_t param)
{
    int result = -1;

    if (bd && bd->graphics_controller && bd->hdmv_vm) {
        GC_NAV_CMDS cmds = {-1, NULL, -1};

        result = gc_run(bd->graphics_controller, msg, param, &cmds);

        if (cmds.num_nav_cmds > 0) {
            hdmv_vm_set_object(bd->hdmv_vm, cmds.num_nav_cmds, cmds.nav_cmds);
            bd->hdmv_suspended = !hdmv_vm_running(bd->hdmv_vm);
        }
    }

    return result;
}

static void _process_hdmv_vm_event(BLURAY *bd, HDMV_EVENT *hev)
{
    BD_DEBUG(DBG_BLURAY, "HDMV event: %d %d\n", hev->event, hev->param);

    switch (hev->event) {
        case HDMV_EVENT_TITLE:
            _close_playlist(bd);
            _play_title(bd, hev->param);
            break;

        case HDMV_EVENT_PLAY_PL:
            bd_select_playlist(bd, hev->param);
            /* initialize menus */
            _run_gc(bd, GC_CTRL_NOP, 0);
            break;

        case HDMV_EVENT_PLAY_PI:
            _queue_event(bd, (BD_EVENT){BD_EVENT_SEEK, 0});
            bd_seek_playitem(bd, hev->param);
            break;

        case HDMV_EVENT_PLAY_PM:
            _queue_event(bd, (BD_EVENT){BD_EVENT_SEEK, 0});
            bd_seek_mark(bd, hev->param);
            break;

        case HDMV_EVENT_PLAY_STOP:
            // stop current playlist
            _close_playlist(bd);
            break;

        case HDMV_EVENT_STILL:
            _queue_event(bd, (BD_EVENT){BD_EVENT_STILL, hev->param});
            break;

        case HDMV_EVENT_ENABLE_BUTTON:
            _run_gc(bd, GC_CTRL_ENABLE_BUTTON, hev->param);
            break;

        case HDMV_EVENT_DISABLE_BUTTON:
            _run_gc(bd, GC_CTRL_DISABLE_BUTTON, hev->param);
            break;

        case HDMV_EVENT_SET_BUTTON_PAGE:
            _run_gc(bd, GC_CTRL_SET_BUTTON_PAGE, hev->param);
            break;

        case HDMV_EVENT_POPUP_OFF:
            _run_gc(bd, GC_CTRL_POPUP, 0);
            break;

        case HDMV_EVENT_IG_END:
            _run_gc(bd, GC_CTRL_IG_END, 0);
            break;

        case HDMV_EVENT_END:
        case HDMV_EVENT_NONE:
        default:
            break;
    }
}

static int _run_hdmv(BLURAY *bd)
{
    HDMV_EVENT hdmv_ev;

    /* run VM */
    if (hdmv_vm_run(bd->hdmv_vm, &hdmv_ev) < 0) {
        _queue_event(bd, (BD_EVENT){BD_EVENT_ERROR, 0});
        bd->hdmv_suspended = !hdmv_vm_running(bd->hdmv_vm);
        return -1;
    }

    /* process all events */
    do {
        _process_hdmv_vm_event(bd, &hdmv_ev);

    } while (!hdmv_vm_get_event(bd->hdmv_vm, &hdmv_ev));

    /* update VM state */
    bd->hdmv_suspended = !hdmv_vm_running(bd->hdmv_vm);

    return 0;
}

int bd_read_ext(BLURAY *bd, unsigned char *buf, int len, BD_EVENT *event)
{
    if (_get_event(bd, event)) {
        return 0;
    }

    /* run HDMV VM ? */
    if (!bd->hdmv_suspended && bd->title_type == title_hdmv) {

        while (!bd->hdmv_suspended) {

            if (_run_hdmv(bd) < 0) {
                BD_DEBUG(DBG_BLURAY|DBG_CRIT, "bd_read_ext(): HDMV VM error\n");
                bd->title_type = title_undef;
                return -1;
            }
            if (_get_event(bd, event)) {
                return 0;
            }
        }
    }

    if (len < 1) {
        /* just polled events ? */
        return 0;
    }

    int bytes = bd_read(bd, buf, len);

    if (bytes == 0) {

        // if no next clip (=end of title), resume HDMV VM
        if (!bd->st0.clip && bd->title_type == title_hdmv) {
            hdmv_vm_resume(bd->hdmv_vm);
            bd->hdmv_suspended = !hdmv_vm_running(bd->hdmv_vm);
            BD_DEBUG(DBG_BLURAY, "bd_read_ext(): reached end of playlist. hdmv_suspended=%d\n", bd->hdmv_suspended);
        }
    }

    _get_event(bd, event);

    return bytes;
}

int bd_get_event(BLURAY *bd, BD_EVENT *event)
{
    if (!bd->event_queue) {
        _init_event_queue(bd);

        bd_psr_register_cb(bd->regs, _process_psr_event, bd);
        _queue_initial_psr_events(bd);
    }

    if (event) {
        return _get_event(bd, event);
    }

    return 0;
}

/*
 * user interaction
 */

void bd_set_scr(BLURAY *bd, int64_t pts)
{
    if (pts >= 0) {
        bd_psr_write(bd->regs, PSR_TIME, (uint32_t)(((uint64_t)pts) >> 1));
    }
}

int bd_mouse_select(BLURAY *bd, int64_t pts, uint16_t x, uint16_t y)
{
    bd_set_scr(bd, pts);

    return _run_gc(bd, GC_CTRL_MOUSE_MOVE, (x << 16) | y);
}

int bd_user_input(BLURAY *bd, int64_t pts, uint32_t key)
{
    bd_set_scr(bd, pts);

    return _run_gc(bd, GC_CTRL_VK_KEY, key);
}

void bd_register_overlay_proc(BLURAY *bd, void *handle, bd_overlay_proc_f func)
{
    if (!bd) {
        return;
    }

    gc_free(&bd->graphics_controller);

    if (func) {
        bd->graphics_controller = gc_init(bd->regs, handle, func);
    }
}

/*
 *
 */

struct meta_dl *bd_get_meta(BLURAY *bd)
{
    if (!bd) {
        return NULL;
    }

    if (!bd->meta) {
        _meta_open(bd);
    }

    uint32_t psr_menu_lang = bd_psr_read(bd->regs, PSR_MENU_LANG);

    if (psr_menu_lang != 0 && psr_menu_lang != 0xffffff) {
        const char language_code[] = {(psr_menu_lang >> 16) & 0xff, (psr_menu_lang >> 8) & 0xff, psr_menu_lang & 0xff, 0 };
        return meta_get(bd->meta, language_code);
    }
    else {
        return meta_get(bd->meta, NULL);
    }
}

struct clpi_cl *bd_get_clpi(BLURAY *bd, unsigned clip_ref)
{
    NAV_CLIP *clip;

    if (bd->title && clip_ref < bd->title->clip_list.count) {
      clip = &bd->title->clip_list.clip[clip_ref];
      CLPI_CL *cl = (CLPI_CL*) calloc(1, sizeof(CLPI_CL));
      return clpi_copy(cl, clip->cl);
    }
    return NULL;
}

void bd_free_clpi(struct clpi_cl *cl)
{
    clpi_free(cl);
}
