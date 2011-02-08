'''Simple FA capture library for reading data from the FA sniffer.'''

DEFAULT_SERVER = 'fa-archiver.cs.diamond.ac.uk'
DEFAULT_PORT = 8888

if __name__ == '__main__':
    from pkg_resources import require
    require('cothread')

import socket
import struct
import numpy
import cothread


MASK_SIZE = 256         # Number of possible bits in a mask

def normalise_mask(mask):
    nmask = numpy.zeros(MASK_SIZE / 8, dtype=numpy.uint8)
    for i in mask:
        nmask[i // 8] |= 1 << (i % 8)
    return nmask

def format_mask(mask):
    '''Converts a mask (a set of integers in the range 0 to 255) into a 64
    character hexadecimal coded string suitable for passing to the server.'''
    return ''.join(['%02X' % x for x in reversed(mask)])

def count_mask(mask):
    n = 0
    for i in range(MASK_SIZE):
        if mask[i // 8] & (1 << (i % 8)):
            n += 1
    return n

class connection:
    class EOF(Exception):
        pass
    class Error(Exception):
        pass

    def __init__(self, server=DEFAULT_SERVER, port=DEFAULT_PORT):
        self.sock = socket.create_connection((server, port))
        self.sock.setblocking(0)
        self.buf = []

    def close(self):
        self.sock.close()

    def recv(self, block_size=65536, timeout=0.2):
        if not cothread.poll_list(
                [(self.sock.fileno(), cothread.POLLIN)], timeout):
            raise socket.timeout('Receive timeout')
        chunk = self.sock.recv(block_size)
        if not chunk:
            raise self.EOF('Connection closed by server')
        return chunk

    def read_block(self, length):
        result = numpy.empty(length, dtype = numpy.int8)
        rx = 0
        buf = self.buf
        while True:
            l = len(buf)
            if l:
                if rx + l <= length:
                    result.data[rx:rx+l] = buf
                    buf = []
                else:
                    result.data[rx:] = buf[:length - rx]
                    buf = buf[length - rx:]
                rx += l
                if rx >= length:
                    break
            buf = self.recv()
        self.buf = buf
        return result


class subscription(connection):
    '''s = subscription(bpm_list, server, port)

    Creates a stream connection to the given FA archiver server (or the default
    server if not specified) returning continuous data for the selected bpms.
    The s.read() method must be called frequently enough to ensure that the
    connection to the server doesn't overflow.
    '''

    def __init__(self, mask, **kargs):
        connection.__init__(self, **kargs)
        self.mask = normalise_mask(mask)
        self.count = count_mask(self.mask)

        self.sock.send('SR%sZ\n' % format_mask(self.mask))
        c = self.recv(1)
        if c != chr(0):
            raise self.Error((c + self.recv())[:-1])    # Discard trailing \n
        self.t0 = struct.unpack('<I', self.recv(4))[0]

    def read(self, samples):
        '''Returns a waveform of samples indexed by sample count, bpm count
        (from the original subscription) and channel, thus:
            wf = s.read(N)
        wf[n, b, x] = sample n of BPM b on channel x, where x=0 for horizontal
        position and x=1 for vertical position.'''
        self.t0 = (self.t0 + samples) & 0xffffffff
        raw = self.read_block(8 * samples * self.count)
        array = numpy.frombuffer(raw, dtype = numpy.int32)
        return array.reshape((samples, self.count, 2))

    def read_t0(self, samples):
        t0 = self.t0
        data = self.read(samples)
        return data, t0


class sample_frequency(connection):
    def __init__(self, **kargs):
        connection.__init__(self, **kargs)
        self.sock.send('CF\n')
        self.frequency = float(self.recv())


def get_sample_frequency(**kargs):
    try:
        return sample_frequency(**kargs).frequency
    except:
        # If get fails fall back to nominal default.
        return 10072.0


if __name__ == '__main__':
    f = sample_frequency()
    import sys
    s = subscription(map(int, sys.argv[1:]))
    while True:
        print numpy.mean(s.read(10000), 0)
