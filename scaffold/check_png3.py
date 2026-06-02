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
# Check center pixel
cx, cy = w//2, h//2
center_start = cy * row_stride + 1 + cx * 4
cp = raw[center_start:center_start+4]
print(f'Center ({cx},{cy}): RGBA({cp[0]},{cp[1]},{cp[2]},{cp[3]})')
# Check top-left
tl_start = 0 * row_stride + 1 + 50 * 4
tl = raw[tl_start:tl_start+4]
print(f'Top-left(50,0): RGBA({tl[0]},{tl[1]},{tl[2]},{tl[3]})')
# Count non-clear pixels
clear = (0,0,26,255)
non_clear = 0
for y in range(h):
    row_start = y * row_stride
    for x in range(0, w, 5):
        px_start = row_start + 1 + x * 4
        px = tuple(raw[px_start:px_start+4])
        if px != clear and px != (0,0,0,0):
            non_clear += 1
print(f'Non-clear sampled pixels: {non_clear}')
