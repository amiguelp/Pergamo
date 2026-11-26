import math
import os
import struct


LAUNCHER_WIDTH = 106
LAUNCHER_HEIGHT = 128
LAUNCHER_ICON_MARGIN = 8


def read_grayscale_bmp(file_path):
    with open(file_path, "rb") as image_file:
        data = image_file.read()

    if len(data) < 54 or data[:2] != b"BM":
        raise ValueError(f"{file_path} is not a valid BMP file")

    pixel_offset = struct.unpack_from("<I", data, 10)[0]
    dib_size = struct.unpack_from("<I", data, 14)[0]
    if dib_size < 40 or len(data) < 14 + dib_size:
        raise ValueError(f"{file_path} has an unsupported BMP header")

    width, signed_height, planes, bits_per_pixel, compression = struct.unpack_from(
        "<iiHHI", data, 18
    )
    colors_used = struct.unpack_from("<I", data, 46)[0]
    if width <= 0 or signed_height == 0 or planes != 1:
        raise ValueError(f"{file_path} has invalid BMP dimensions")
    if bits_per_pixel != 8 or compression != 0:
        raise ValueError(f"{file_path} must be an uncompressed 8-bit BMP")

    height = abs(signed_height)
    palette_count = colors_used or 256
    palette_offset = 14 + dib_size
    if palette_count > 256 or palette_offset + palette_count * 4 > pixel_offset:
        raise ValueError(f"{file_path} has an invalid BMP palette")

    palette = []
    for index in range(palette_count):
        blue, green, red, _ = struct.unpack_from(
            "<BBBB", data, palette_offset + index * 4
        )
        palette.append((299 * red + 587 * green + 114 * blue + 500) // 1000)

    row_stride = (width + 3) & ~3
    if pixel_offset + row_stride * height > len(data):
        raise ValueError(f"{file_path} pixel data is truncated")

    pixels = []
    for y in range(height):
        file_y = y if signed_height < 0 else height - 1 - y
        row_offset = pixel_offset + file_y * row_stride
        row = data[row_offset:row_offset + width]
        if any(index >= palette_count for index in row):
            raise ValueError(f"{file_path} contains an invalid palette index")
        pixels.extend(palette[index] for index in row)

    return pixels, width, height


def resize_grayscale(source, source_width, source_height, width, height):
    resized = bytearray(width * height)
    for y in range(height):
        source_y = max(0.0, (y + 0.5) * source_height / height - 0.5)
        y0 = int(math.floor(source_y))
        y1 = min(y0 + 1, source_height - 1)
        fy = source_y - y0
        for x in range(width):
            source_x = max(0.0, (x + 0.5) * source_width / width - 0.5)
            x0 = int(math.floor(source_x))
            x1 = min(x0 + 1, source_width - 1)
            fx = source_x - x0
            top = source[y0 * source_width + x0] * (1 - fx) + source[y0 * source_width + x1] * fx
            bottom = source[y1 * source_width + x0] * (1 - fx) + source[y1 * source_width + x1] * fx
            resized[y * width + x] = int(round(top * (1 - fy) + bottom * fy))
    return resized


def write_grayscale_bmp(file_path, pixels):
    row_stride = (LAUNCHER_WIDTH + 3) & ~3
    pixel_data_size = row_stride * LAUNCHER_HEIGHT
    pixel_offset = 14 + 40 + 256 * 4
    file_size = pixel_offset + pixel_data_size

    file_header = struct.pack("<2sIHHI", b"BM", file_size, 0, 0, pixel_offset)
    dib_header = struct.pack(
        "<IiiHHIIiiII", 40, LAUNCHER_WIDTH, LAUNCHER_HEIGHT, 1, 8, 0,
        pixel_data_size, 2835, 2835, 256, 256
    )
    palette = b"".join(bytes((value, value, value, 0)) for value in range(256))

    with open(file_path, "wb") as image_file:
        image_file.write(file_header)
        image_file.write(dib_header)
        image_file.write(palette)
        for y in range(LAUNCHER_HEIGHT - 1, -1, -1):
            row = pixels[y * LAUNCHER_WIDTH:(y + 1) * LAUNCHER_WIDTH]
            image_file.write(row)
            image_file.write(b"\0" * (row_stride - LAUNCHER_WIDTH))


def generate_launcher_icons(source_path):
    source, source_width, source_height = read_grayscale_bmp(source_path)
    content_size = min(LAUNCHER_WIDTH, LAUNCHER_HEIGHT) - 2 * LAUNCHER_ICON_MARGIN
    scale = min(content_size / source_width, content_size / source_height)
    width = max(1, int(round(source_width * scale)))
    height = max(1, int(round(source_height * scale)))
    resized = resize_grayscale(source, source_width, source_height, width, height)

    normal = bytearray([255]) * (LAUNCHER_WIDTH * LAUNCHER_HEIGHT)
    focused = bytearray(normal)
    x_offset = (LAUNCHER_WIDTH - width) // 2
    y_offset = (LAUNCHER_HEIGHT - height) // 2
    for y in range(height):
        for x in range(width):
            value = resized[y * width + x]
            index = (y + y_offset) * LAUNCHER_WIDTH + x + x_offset
            normal[index] = value
            focused[index] = 255 - value

    write_grayscale_bmp("Pergamo.bmp", normal)
    write_grayscale_bmp("Pergamo_f.bmp", focused)


def write_c_array(file_path, array_name, out_file):
    if not os.path.exists(file_path):
        print(f"Error: Could not find {file_path}")
        return

    with open(file_path, "rb") as f:
        data = f.read()

    out_file.write(f"// Generated from {file_path}\n")
    out_file.write(f"const unsigned char {array_name}[] = {{\n")
    
    hex_data = [f"0x{b:02x}" for b in data]
    for i in range(0, len(hex_data), 12):
        out_file.write("    " + ", ".join(hex_data[i:i+12]) + ",\n")
        
    out_file.write("};\n")
    out_file.write(f"const unsigned int {array_name}_len = {len(data)};\n\n")
    print(f"Successfully converted {file_path} to {array_name} array.")

if __name__ == "__main__":
    generate_launcher_icons("opds.bmp")

    with open("icons.h", "w") as out_file:
        out_file.write("#ifndef ICONS_H\n#define ICONS_H\n\n")
        
        write_c_array("folder.png", "folder_icon_png", out_file)
        write_c_array("book.png", "book_icon_png", out_file)
        
        out_file.write("#endif // ICONS_H\n")

