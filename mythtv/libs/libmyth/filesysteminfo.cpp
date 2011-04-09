
using namespace std;

#include <QList>
#include <QString>
#include <QStringList>

#include "filesysteminfo.h"

// for serialization
#define INT_TO_LIST(x)       do { list << QString::number(x); } while (0)
#define DATETIME_TO_LIST(x)  INT_TO_LIST((x).toTime_t())
#define STR_TO_LIST(x)       do { list << (x); } while (0)

// for deserialization
#define NEXT_STR()        do { if (it == listend)                    \
                               {                                     \
                                   VERBOSE(VB_IMPORTANT, listerror); \
                                   clear();                          \
                                   return false;                     \
                               }                                     \
                               ts = *it++; } while (0)
#define INT_FROM_LIST(x)     do { NEXT_STR(); (x) = ts.toLongLong(); } while (0)
#define ENUM_FROM_LIST(x, y) do { NEXT_STR(); (x) = ((y)ts.toInt()); } while (0)
#define DATETIME_FROM_LIST(x) \
    do { NEXT_STR(); (x).setTime_t(ts.toUInt()); } while (0)
#define STR_FROM_LIST(x)     do { NEXT_STR(); (x) = ts; } while (0)

#define LOC QString("FileSystemInfo: ")
#define LOC_ERR QString("FileSystemInfo, error: ")

FileSystemInfo::FileSystemInfo(void) :
    m_hostname(""), m_path(""), m_local(false), m_fsid(-1),
    m_grpid(-1), m_blksize(4096), m_total(0), m_used(0)
{
}

FileSystemInfo::FileSystemInfo(const FileSystemInfo &other)
{
    clone(other);
}

FileSystemInfo::FileSystemInfo(QString hostname, QString path, bool local,
        int fsid, int groupid, int blksize, long long total, long long used) :
    m_hostname(hostname), m_path(path), m_local(local), m_fsid(fsid),
    m_grpid(groupid), m_blksize(blksize), m_total(total), m_used(used)
{
}

FileSystemInfo::FileSystemInfo(QStringList::const_iterator &it,
                   QStringList::const_iterator end)
{
    FromStringList(it, end);
}

FileSystemInfo::FileSystemInfo(const QStringList &slist)
{
    FromStringList(slist);
}

void FileSystemInfo::clone(const FileSystemInfo &other)
{
    m_hostname = other.m_hostname;
    m_path = other.m_path;
    m_local = other.m_local;
    m_fsid = other.m_fsid;
    m_grpid = other.m_grpid;
    m_blksize = other.m_blksize;
    m_total = other.m_total;
    m_used = other.m_used;
}

FileSystemInfo &FileSystemInfo::operator=(const FileSystemInfo &other)
{
    clone(other);
    return *this;
}

void FileSystemInfo::clear(void)
{
    m_hostname  = "";
    m_path      = "";
    m_local     = false;
    m_fsid      = -1;
    m_grpid     = -1;
    m_blksize   = 4096;
    m_total     = 0;
    m_used      = 0;
}

bool FileSystemInfo::ToStringList(QStringList &list) const
{
    STR_TO_LIST(m_hostname);
    STR_TO_LIST(m_path);
    INT_TO_LIST(m_local);
    INT_TO_LIST(m_fsid);
    INT_TO_LIST(m_grpid);
    INT_TO_LIST(m_blksize);
    INT_TO_LIST(m_total);
    INT_TO_LIST(m_used);

    return true;
}

bool FileSystemInfo::FromStringList(const QStringList &slist)
{
    QStringList::const_iterator it = slist.constBegin();
    return FromStringList(it, slist.constEnd());
}

bool FileSystemInfo::FromStringList(QStringList::const_iterator &it,
                             QStringList::const_iterator listend)
{
    QString listerror = LOC + "FromStringList, not enough items in list.";
    QString ts;

    STR_FROM_LIST(m_hostname);
    STR_FROM_LIST(m_path);
    INT_FROM_LIST(m_local);
    INT_FROM_LIST(m_fsid);
    INT_FROM_LIST(m_grpid);
    INT_FROM_LIST(m_blksize);
    INT_FROM_LIST(m_total);
    INT_FROM_LIST(m_used);

    return true;
}

const QList<FileSystemInfo> FileSystemInfo::RemoteGetInfo(MythSocket *sock)
{
    FileSystemInfo fsInfo;
    QList<FileSystemInfo> fsInfos;
    QStringList strlist(QString("QUERY_FREE_SPACE_LIST"));

    bool sent;

    if (sock)
        sent = sock->SendReceiveStringList(strlist);
    else
        sent = gCoreContext->SendReceiveStringList(strlist);

    if (sent)
    {
        int numdisks = strlist.size()/NUMDISKINFOLINES;

        QStringList::const_iterator it = strlist.begin();
        for (int i = 0; i < numdisks; i++)
        {
            fsInfo.FromStringList(it, strlist.end());
            fsInfos.append(fsInfo);
        }
    }

    return fsInfos;
}