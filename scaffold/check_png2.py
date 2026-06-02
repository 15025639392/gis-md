import struct, zlib
with open('scaffold/screenshot_fixed.png', 'rb') as f:
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
colors = set()
for y in range(0, min(h, 100)):
    row_start = y * row_stride
    for x in range(0, w, 30):
        px_start = row_start + 1 + x * 4
        px = tuple(raw[px_start:px_start+4])
        colors.add(px)
print(f'Unique colors: {len(colors)}')
for c in sorted(colors)[:30]:
    print(f'  RGBA({c[0]}, {c[1]}, {c[2]}, {c[3]})')
