/*
 *  Copyright (C) Kenneth Aafloy 2003
 *  
 *  Copyright notice is in dvbrecorder.cpp of the MythTV project.
 */

#ifndef DVBRECORDER_H
#define DVBRECORDER_H

#include <vector>
#include <map>
using namespace std;

#include "dtvrecorder.h"
#include "transform.h"

#include "dvbtypes.h"
#include "dvbchannel.h"
#include "dvbsiparser.h"

class DVBRecorder: public QObject, public DTVRecorder
{
    Q_OBJECT
public:
    DVBRecorder(DVBChannel* dvbchannel);
   ~DVBRecorder();

    void SetOption(const QString &name, int value);

    void SetOptionsFromProfile(RecordingProfile *profile,
                               const QString &videodev,
                               const QString &audiodev,
                               const QString &vbidev, int ispip);

    void StartRecording(void);
    void Reset(void);

public slots:
    void ChannelChanged(dvb_channel_t& chan);

private:
    bool Open();
    void Close();

    void ReadFromDMX();
    static void ProcessDataPS(unsigned char *buffer, int len, void *priv);
    void LocalProcessDataPS(unsigned char *buffer, int len);
    void LocalProcessDataTS(unsigned char *buffer, int len);

    void CloseFilters();
    void OpenFilters(uint16_t pid, ES_Type type);
    void SetDemuxFilters();
    void AutoPID();

    void CreatePAT(uint8_t *ts_packet);
    void CreatePMT(uint8_t *ts_packet);

    // Options set in SetOption()
    int  _card_number_option;
    bool _record_transport_stream_option;
    bool _dvb_on_demand_option;
    bool _software_filter_option;

    // DVB stuff
    DVBChannel*     dvbchannel;
    pid_ipack_t     pid_ipack;
    PMTObject       m_pmt;
    unsigned char   prvpkt[3];
    vector<int>     _pid_filters;
    bool            _software_filter_open;
    bool            _reset_pid_filters; // set when we want to generate a new filter set

    uint8_t *pat_pkt;
    uint8_t *pmt_pkt;
    static const int PMT_PID;
    int pat_cc;
    int pmt_cc;
    int pkts_until_pat_pmt;

    // statistics
    unsigned int _continuity_error_count;
    unsigned int _stream_overflow_count;
    unsigned int _bad_packet_count;
    map<uint16_t,uint8_t> _continuity_count;
    map<uint16_t,bool> pusi_seen;
    map<uint16_t,bool> isVideo;

    // for debugging
    void DebugTSHeader(unsigned char* buffer, int len);
    bool data_found; // debugging variable used by transform.c
    bool keyframe_found;

    // to see when CAM starts working
    map<uint16_t,bool> scrambled;
};

#endif
