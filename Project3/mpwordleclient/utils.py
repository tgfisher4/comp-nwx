import socket
import json

# nl (newline) protocol
BUFSIZ = 1024
def nl_messages(get_fxn):
    ''' "Gets" and returns a newline delimited message from a function.
        'from_fxn' should be a function accepting one integer argument ('size')
        and returning a byte-string up to length 'size'.
    '''
    # Use generator to maintain state between calls in case we receive too many bytes.
    # E.g.: many messages waiting to be received, many fitting within 1024 bytes.
    #print("generator init")
    extra = b''
    while piece := get_fxn():
        extra += piece
        while b'\n' in extra:
            msg, extra = extra.split(b'\n', maxsplit=1)
            yield msg            

def nl_file_messages(from_file):
    ''' Reads a newline delimited message from a file. '''
    yield from nl_messages(lambda: from_file.read(BUFSIZ))

def nl_socket_messages(from_skt):
    ''' Receives a newline delimited message from a socket. '''
    yield from nl_messages(lambda: print("calling recv") or from_skt.recv(BUFSIZ))
    #print(type(rlt))
    #return rlt

def put_item(put_fxn, item):
    ''' "Puts" a byte string 'item' by passing it to 'put_fxn'. '''
    # Bit roundabout since there's no extra logic here as in get_item.
    # However, can easily facilitate adding extra logic to ALL fxns that
    # put (send/write) items in one place.
    #print(item)
    return put_fxn(item)

def write_item_2(to_file, item):
    ''' Writes byte string 'item' to a file opened for binary writing. '''
    return put_item(lambda item: to_file.write(item), item)

def send_item_2(to_skt, item):
    ''' Sends byte string 'item' to a socket. '''
    return put_item(lambda item: to_skt.sendall(item), item)

def build_nl_packet(raw_pyld):
    ''' Builds an nl protocl packet containing the payload 'raw_pyld'. '''
    return raw_pyld + b'\n'

def send_nl_message(to_skt, msg):
    send_message(to_skt, msg, build_nl_packet)

def send_message(to_skt, msg, packet_builder):
    send_item_2(to_skt, packet_builder(msg))

def decode_object(raw_obj):
    ''' Convert bytes into app-level communication message Python data structure via JSON. '''
    try:
        return json.loads(raw_obj.decode('utf-8'))
    except (json.decoder.JSONDecodeError, UnicodeDecodeError) as e:
        raise RequestFormatError('object is not valid JSON encoded in utf-8') from e

def encode_object(obj):
    ''' Convert app-level communication message Python data structure into bytes via JSON. '''
    return json.dumps(obj).encode('utf-8') # bubbles stringifying exceptions
