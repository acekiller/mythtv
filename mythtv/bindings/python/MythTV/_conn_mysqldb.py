from logging import MythLog
from exceptions import MythDBError

from weakref import ref

import sys
import warnings
with warnings.catch_warnings():
    warnings.simplefilter('ignore')
    import MySQLdb, MySQLdb.cursors
if (MySQLdb.version_info < (1,2,3)) and (sys.version_info >= (2,7)):
    raise ImportError('MySQLdb is too old for this version of Python')

__version__ = tuple(['MySQLdb']+list(MySQLdb.version_info))

def dbconnect(dbconn, log):
    log(MythLog.DATABASE, MythLog.INFO,
                    'Spawning new database connection')
    db = MySQLdb.connect(  user=   dbconn['DBUserName'],
                           host=   dbconn['DBHostName'],
                           passwd= dbconn['DBPassword'],
                           db=     dbconn['DBName'],
                           port=   dbconn['DBPort'],
                           use_unicode=True,
                           charset='utf8')
    db.autocommit(True)
    return db

class LoggedCursor( MySQLdb.cursors.Cursor ):
    """
    Custom cursor, offering logging and error handling
    """
    def __init__(self, connection):
        super(LoggedCursor, self).__init__(connection)
        self.log = None
        self.ping = ref(self._ping121)
        if MySQLdb.version_info >= ('1','2','2'):
            self.ping = ref(self._ping122)
        self.ping()

    def _ping121(self): self.connection.ping(True)
    def _ping122(self): self.connection.ping()

    def _sanitize(self, query): return query.replace('?', '%s')

    def log_query(self, query, args):
        self.log(self.log.DATABASE, MythLog.DEBUG, 
                 ' '.join(query.split()), str(args))

    def execute(self, query, args=None):
        """
        Execute a query.

        query -- string, query to execute on server
        args -- optional sequence or mapping, parameters to use with query.

        Note: If args is a sequence, then %s must be used as the
        parameter placeholder in the query. If a mapping is used,
        %(key)s must be used as the placeholder.

        Returns long integer rows affected, if any
        """
        self.ping()
        query = self._sanitize(query)
        self.log_query(query, args)
        try:
            if args is None:
                return super(LoggedCursor, self).execute(query)
            return super(LoggedCursor, self).execute(query, args)
        except Exception, e:
            raise MythDBError(MythDBError.DB_RAW, e.args)

    def executemany(self, query, args):
        """
        Execute a multi-row query.

        query -- string, query to execute on server

        args

            Sequence of sequences or mappings, parameters to use with
            query.

        Returns long integer rows affected, if any.

        This method improves performance on multiple-row INSERT and
        REPLACE. Otherwise it is equivalent to looping over args with
        execute().
        """
        self.ping()
        query = self._sanitize(query)
        self.log_query(query, args)
        try:
            return super(LoggedCursor, self).executemany(query, args)
        except Exception, e:
            raise MythDBError(MythDBError.DB_RAW, e.args)

    def commit(self): self.connection.commit()
    def rollback(self): self.connection.rollback()

    def __enter__(self):
        return self
    def __exit__(self, type, value, traceback):
        if type:
            self.rollback()
        else:
            self.commit()
        self.close()
