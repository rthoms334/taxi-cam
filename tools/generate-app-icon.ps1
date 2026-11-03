[CmdletBinding()]
param([string]$Output = (Join-Path $PSScriptRoot '../src/app/taxi-cam.ico'))
$ErrorActionPreference = 'Stop'
Add-Type -TypeDefinition @'
using System;
using System.IO;
public static class TaxiCamIcon {
    // Fixed 3x5 block glyphs avoid machine-dependent font rasterization.
    static readonly byte[,] Letters = {
        {7,2,2,2,2}, // T
        {2,5,7,5,5}, // A
        {5,5,2,5,5}, // X
        {7,2,2,2,7}  // I
    };
    static bool RoundedSquare(double x, double y, double halfExtent, double radius) {
        double dx = Math.Max(Math.Abs(x - 16) - halfExtent + radius, 0);
        double dy = Math.Max(Math.Abs(y - 16) - halfExtent + radius, 0);
        return dx * dx + dy * dy < radius * radius;
    }
    static bool TaxiLabel(double x, double y) {
        if (x < 4 || x >= 28 || y < 19.5 || y >= 27.5) return false;
        int column = (int)((x - 4) / 1.6), row = (int)((y - 19.5) / 1.6);
        int letter = column / 4, pixel = column % 4;
        return pixel < 3 && (Letters[letter,row] & (1 << (2 - pixel))) != 0;
    }
    static bool Markings(double x, double y) {
        // Two continuous hold-short lines below, two dashed lines beyond.
        // The asphalt mask clips each full-width bar before the teal border.
        if (Math.Abs(y - 11) <= 0.6 || Math.Abs(y - 13) <= 0.6) return true;
        if ((Math.Abs(y - 7) <= 0.6 || Math.Abs(y - 9) <= 0.6) &&
            ((x + 0.75) % 4.75 <= 3.5)) return true;
        return TaxiLabel(x, y);
    }
    static byte[] Image(int size) {
        var pixels = new uint[size * size];
        for (int y = 0; y < size; ++y) for (int x = 0; x < size; ++x) {
            int count = 0, r = 0, g = 0, b = 0;
            for (int sy = 0; sy < 8; ++sy) for (int sx = 0; sx < 8; ++sx) {
                double px = (x + (sx + 0.5) / 8) * 32 / size;
                double py = (y + (sy + 0.5) / 8) * 32 / size;
                uint color = RoundedSquare(px, py, 15.5, 6) ? 0xff42dbb8u : 0;
                if (RoundedSquare(px, py, 14.7, 5.2)) color = Markings(px, py) ? 0xffffd633u : 0xff11151cu;
                if (color != 0) { ++count; r += (int)((color >> 16) & 255); g += (int)((color >> 8) & 255); b += (int)(color & 255); }
            }
            if (count != 0) pixels[y*size+x] = (uint)(((count*255+32)/64) << 24 | (r/count) << 16 | (g/count) << 8 | b/count);
        }
        using (var data = new MemoryStream()) using (var writer = new BinaryWriter(data)) {
            int stride = ((size + 31) / 32) * 4;
            writer.Write(40); writer.Write(size); writer.Write(size * 2);
            writer.Write((ushort)1); writer.Write((ushort)32); writer.Write(0);
            writer.Write(size * size * 4 + stride * size);
            writer.Write(0); writer.Write(0); writer.Write(0); writer.Write(0);
            for (int y = size - 1; y >= 0; --y) for (int x = 0; x < size; ++x) writer.Write(pixels[y*size+x]);
            for (int y = size - 1; y >= 0; --y) {
                var mask = new byte[stride];
                for (int x = 0; x < size; ++x) if ((pixels[y*size+x] >> 24) == 0) mask[x/8] |= (byte)(128 >> (x%8));
                writer.Write(mask);
            }
            return data.ToArray();
        }
    }
    public static void Write(string path) {
        int[] sizes = {16,20,24,32,40,48,64,128,256};
        var images = new byte[sizes.Length][];
        for (int i = 0; i < sizes.Length; ++i) images[i] = Image(sizes[i]);
        using (var writer = new BinaryWriter(File.Create(path))) {
            writer.Write((ushort)0); writer.Write((ushort)1); writer.Write((ushort)sizes.Length);
            int offset = 6 + 16 * sizes.Length;
            for (int i = 0; i < sizes.Length; ++i) {
                writer.Write((byte)(sizes[i] == 256 ? 0 : sizes[i])); writer.Write((byte)(sizes[i] == 256 ? 0 : sizes[i]));
                writer.Write((byte)0); writer.Write((byte)0); writer.Write((ushort)1); writer.Write((ushort)32);
                writer.Write(images[i].Length); writer.Write(offset); offset += images[i].Length;
            }
            foreach (var image in images) writer.Write(image);
        }
    }
}
'@
[TaxiCamIcon]::Write([IO.Path]::GetFullPath($Output))
Write-Output "Generated $Output (16, 20, 24, 32, 40, 48, 64, 128 and 256 pixels)."
