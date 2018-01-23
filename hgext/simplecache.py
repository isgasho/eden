# simplecache.py - cache slow things locally so they are fast the next time
#
# Copyright 2014 Facebook, Inc.
#
# This software may be used and distributed according to the terms of the
# GNU General Public License version 2 or any later version.

"""
simplecache is a dirt-simple cache of various functions that get slow in large
repositories. It is aimed at speeding up common operations that programmers
often take, like diffing two revisions (eg, hg export).

Currently we cache the full results of these functions:
    copies.pathcopies (a dictionary)
    context.basectx._buildstatus (a scmutil.status object -- a tuple of lists)

You can disable its debug statements (defaults to 'on' except in tests)::

  [simplecache]
  showdebug = False
"""

import socket, json, random, os, tempfile
from mercurial import (
    context,
    copies,
    encoding,
    extensions,
    node,
)
from mercurial.node import (
    nullid,
    wdirid
)
from mercurial.scmutil import status

testedwith = 'ships-with-fb-hgext'

# context nodes that are special and should not be cached.
UNCACHEABLE_NODES = [
    None, # repo[None].node() returns this
    nullid,
    wdirid
]

def extsetup(ui):
    extensions.wrapfunction(copies, 'pathcopies', pathcopiesui(ui))
    extensions.wrapfunction(context.basectx, '_buildstatus', buildstatusui(ui))

def getmcsock(ui):
    """
    Return a socket opened up to talk to localhost mcrouter.
    """
    s = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    host = ui.config('simplecache', 'host', default='localhost')
    port = int(ui.config('simplecache', 'port', default=11101))
    s.connect((host, port))
    return s

def mcget(key, ui):
    """
    Use local mcrouter to get a key from memcache
    """
    if type(key) != str:
        raise ValueError('Key must be a string')
    s = getmcsock(ui)
    key = 'cca.hg.%s' % key
    s.sendall('get %s\r\n' % key)
    meta = []
    value = None
    while True:
        char = s.recv(1)
        if char != '\r':
            meta.append(char)
        else:
            meta = ''.join(meta)
            if meta == 'END':
                break
            char = s.recv(1) # throw away newline
            _, key, flags, sz  = ''.join(meta).strip().split(' ')
            value = s.recv(int(sz))
            s.recv(7) # throw away \r\nEND\r\n
            break
    s.close()
    return value

def mcset(key, value, ui):
    """
    Use local mcrouter to set a key to memcache
    """
    if type(key) != str:
        raise ValueError('Key must be a string')
    if type(value) != str:
        raise ValueError('Value must be a string')

    key = 'cca.hg.%s' % key
    sz = len(value)
    tmpl = 'set %s 0 0 %d\r\n%s\r\n'
    s = getmcsock(ui)
    s.sendall(tmpl % (key, sz, value))
    data = []
    while True:
        char = s.recv(1)
        if char not in '\r\n':
            data.append(char)
        else:
            break
    s.close()
    return ''.join(data) == 'STORED'

class pathcopiesserializer(object):
    """
    Serialize and deserialize the results of calls to copies.pathcopies.
    Results are just dictionaries, so this just uses json.
    """
    @staticmethod
    def serialize(copydict):
        encoded = dict((k.encode('base64'), v.encode('base64'))
                for (k, v) in copydict.iteritems())
        return json.dumps(encoded)

    @staticmethod
    def deserialize(string):
        encoded = json.loads(string)
        return dict((k.decode('base64'), v.decode('base64'))
                for k, v in encoded.iteritems())

def pathcopiesui(ui):
    def pathcopies(orig, x, y, match=None):
        func = lambda: orig(x, y, match=match)
        if (x.node() not in UNCACHEABLE_NODES and y.node()
            not in UNCACHEABLE_NODES and not match):
            key = 'pathcopies:%s:%s' % (
                    node.hex(x.node()), node.hex(y.node()))
            return memoize(func, key, pathcopiesserializer, ui)
        return func()
    return pathcopies

class buildstatusserializer(object):
    """
    Serialize and deserialize the results of calls to buildstatus.
    Results are status objects, which extend tuple. Each status object
    has seven lists within it, each containing strings of filenames in
    each type of status.
    """
    @staticmethod
    def serialize(status):
        ls = [list(status[i]) for i in range(7)]
        ll = []
        for s in ls:
            ll.append([f.encode('base64') for f in s])
        return json.dumps(ll)

    @staticmethod
    def deserialize(string):
        ll = json.loads(string)
        ls = []
        for l in ll:
            ls.append([f.decode('base64') for f in l])
        return status(*ls)

def buildstatusui(ui):
    def buildstatus(orig, self, other, status, match, ignored, clean, unknown):
        func = lambda: orig(self, other, status, match, ignored, clean, unknown)
        if not match.always():
            return func()
        if ignored or clean or unknown:
            return func()
        if (self.node() in UNCACHEABLE_NODES or
            other.node() in UNCACHEABLE_NODES):
            return func()
        key = 'buildstatus:%s:%s' % (
                node.hex(self.node()), node.hex(other.node()))
        return memoize(func, key, buildstatusserializer, ui)

    return buildstatus

class stringserializer(object):
    """Simple serializer that just checks if the input is a string and returns
    it.
    """
    @staticmethod
    def serialize(input):
        if type(input) is not str:
            raise TypeError("stringserializer can only be used with strings")
        return input

    @staticmethod
    def deserialize(string):
        if type(string) is not str:
            raise TypeError("stringserializer can only be used with strings")
        return string

def localpath(key, ui):
    tempdir = ui.config('simplecache', 'cachedir')
    if not tempdir:
        tempdir = os.path.join(tempfile.gettempdir(), 'hgsimplecache')
    return os.path.join(tempdir, key)

def localget(key, ui):
    try:
        path = localpath(key, ui)
        with open(path) as f:
            return f.read()
    except Exception:
        return None

def localset(key, value, ui):
    try:
        path = localpath(key, ui)
        dirname = os.path.dirname(path)
        if not os.path.exists(dirname):
            os.makedirs(dirname)
        with open(path, 'w') as f:
            f.write(value)

        # If too many entries in cache, delete some.
        tempdirpath = localpath('', ui)
        entries = os.listdir(tempdirpath)
        maxcachesize = ui.configint('simplecache', 'maxcachesize', 2000)
        if len(entries) > maxcachesize:
            random.shuffle(entries)
            evictionpercent = ui.configint('simplecache', 'evictionpercent', 50)
            evictionpercent /= 100.0
            for i in xrange(0, int(len(entries) * evictionpercent)):
                os.remove(os.path.join(tempdirpath, entries[i]))
    except Exception:
        return

cachefuncs = {
    'local' : (localget, localset),
    'memcache' : (mcget, mcset),
}

def memoize(func, key, serializer, ui):
    version = ui.config('simplecache', 'version', default='1')
    key = "%s:v%s" % (key, version)
    cachelist = ui.configlist('simplecache', 'caches', ['local'])
    for name in cachelist:
        get, set = cachefuncs[name]
        try:
            cacheval = get(key, ui)
            if cacheval is not None:
                _debug(ui, 'got value for key %s from %s\n' % (key, name))
                value = serializer.deserialize(cacheval)
                return value
        except Exception as inst:
            _debug(ui, 'error getting or deserializing key %s: %s\n'
                     % (key, inst))

    _debug(ui, 'falling back for value %s\n' % (key))
    value = func()

    for name in cachelist:
        get, set = cachefuncs[name]
        try:
            set(key, serializer.serialize(value), ui)
            _debug(ui, 'set value for key %s to %s\n' % (key, name))
        except Exception as inst:
            _debug(ui, 'error setting key %s: %s\n' % (key, inst))

    return value

def _runningintests():
    return 'TESTTMP' in encoding.environ

def _debug(ui, msg):
    config = ui.configbool('simplecache', 'showdebug', None)
    if config is None:
        config = not _runningintests()

    if config:
        ui.debug(msg)
