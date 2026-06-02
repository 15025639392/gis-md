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

# Check different rows
for y in [1300, 1386, 1500, 1800, 2000, 2500]:
    if y < h:
        px_start = y * row_stride + 1 + 620 * 4
        px = raw[px_start:px_start+4]
        print(f'Row {y}, col 620: RGBA({px[0]},{px[1]},{px[2]},{px[3]})')

# Find first non-black row
for y in range(h):
    row_start = y * row_stride + 1
    px = tuple(raw[row_start+620*4:row_start+620*4+4])
    if px != (0,0,0,0) and px != (0,0,26,255):
        print(f'First non-background row: {y}, col620: RGBA{px}')
        break
