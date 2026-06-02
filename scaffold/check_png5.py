import struct, zlib
with open('scaffold/screenshot_final2.png', 'rb') as f:
    data = f.read()
pos = 8
idat_data = b''
while pos < len(data):
    length = struct.unpack('>I', data[pos:pos+4])[0]
    chunk_type = data[pos+4:pos+8]
    if chunk_type == b'IDAT':
        idat_data += data[pos+8:pos+8+length]
    pos += 12 + length
raw = zlib.decompress(idat_data)
w, h = 1240, 2772
row_stride = w * 4 + 1

from collections import Counter
rgb_counter = Counter()
for y in range(h):
    row_start = y * row_stride
    for x in range(0, w, 10):
        px_start = row_start + 1 + x * 4
        r, g, b, a = raw[px_start], raw[px_start+1], raw[px_start+2], raw[px_start+3]
        if a > 0 and (r > 30 or g > 30 or b > 30):
            rgb_counter[(r//50*50, g//50*50, b//50*50)] += 1
            
print(f'Bright pixels (a>0, rgb>30): {sum(rgb_counter.values())}')
print('Top 10 color clusters:')
for (r,g,b), count in rgb_counter.most_common(10):
    print(f'  ~RGB({r},{g},{b}): {count}')
