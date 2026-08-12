using System;
using System.Collections.Generic;
using System.IO;
using System.Text;
using System.Windows.Media.Imaging;

/// <summary>
/// Extract all images from a DDSystem DDP2/DDP3 archive.
/// Usage: extract_ddp.exe &lt;archive.dat&gt; &lt;out_dir&gt;
/// Saves each image entry as &lt;entry_name&gt;.bmp in out_dir.
/// Prints one line per extracted image:  &lt;entry_name&gt; TAB &lt;width&gt; TAB &lt;height&gt;
///
/// Parses the archive index natively (no GARbro needed for indexing).
/// Decompresses entries with ShsCompression (ported from GARbro ArcHXP.cs).
/// Decodes BMP images via System.Windows.Media.Imaging.
/// </summary>
class ExtractDdp
{
    // -----------------------------------------------------------------------
    // ShsCompression  (ported from GARbro ArcHXP.cs, ShsCompression class)
    // -----------------------------------------------------------------------
    class ShsReader
    {
        readonly byte[] _data;
        int _pos;

        public ShsReader(byte[] data) { _data = data; _pos = 0; }

        int ReadU8()  { return _data[_pos++] & 0xFF; }
        int ReadU16BE()
        {
            int v = ((_data[_pos] & 0xFF) << 8) | (_data[_pos+1] & 0xFF);
            _pos += 2;
            return v;
        }
        int ReadI32BE()
        {
            int v = ((_data[_pos] & 0xFF) << 24)
                  | ((_data[_pos+1] & 0xFF) << 16)
                  | ((_data[_pos+2] & 0xFF) << 8)
                  | (_data[_pos+3] & 0xFF);
            _pos += 4;
            return v;
        }

        public void Unpack(byte[] output)
        {
            int dst = 0;
            while (dst < output.Length)
            {
                int count;
                int ctl = ReadU8();
                if (ctl < 32)
                {
                    if (ctl == 0x1D)
                        count = ReadU8() + 0x1E;
                    else if (ctl == 0x1E)
                        count = ReadU16BE() + 0x11E;
                    else if (ctl == 0x1F)
                        count = ReadI32BE();
                    else
                        count = ctl + 1;

                    count = Math.Min(count, output.Length - dst);
                    Array.Copy(_data, _pos, output, dst, count);
                    _pos += count;
                }
                else
                {
                    int offset;
                    if (0 == (ctl & 0x80))
                    {
                        if (0x20 == (ctl & 0x60))
                        {
                            offset = (ctl >> 2) & 7;
                            count  = ctl & 3;
                        }
                        else
                        {
                            offset = ReadU8();
                            if (0x40 == (ctl & 0x60))
                            {
                                count = (ctl & 0x1F) + 4;
                            }
                            else
                            {
                                offset |= (ctl & 0x1F) << 8;
                                ctl = ReadU8();
                                if (0xFE == ctl)
                                    count = ReadU16BE() + 0x102;
                                else if (0xFF == ctl)
                                    count = ReadI32BE();
                                else
                                    count = ctl + 4;
                            }
                        }
                    }
                    else
                    {
                        count  = (ctl >> 5) & 3;
                        offset = ((ctl & 0x1F) << 8) | ReadU8();
                    }
                    count += 3;
                    offset++;
                    count = Math.Min(count, output.Length - dst);
                    CopyOverlapped(output, dst - offset, dst, count);
                }
                dst += count;
            }
        }

        static void CopyOverlapped(byte[] data, int src, int dst, int count)
        {
            for (int i = 0; i < count; i++)
                data[dst + i] = data[src + i];
        }
    }

    // -----------------------------------------------------------------------
    // Entry descriptor
    // -----------------------------------------------------------------------
    struct Entry
    {
        public long   Offset;
        public int    UnpackedSize;
        public int    PackedSize;
        public string Name;      // already sanitised for the filesystem
        public string RawName;   // original name (for stdout)
    }

    // -----------------------------------------------------------------------
    // Index parsers
    // -----------------------------------------------------------------------

    static List<Entry> ParseDdp2(string arcPath)
    {
        var entries = new List<Entry>();
        using (var f = new FileStream(arcPath, FileMode.Open, FileAccess.Read, FileShare.Read))
        using (var br = new BinaryReader(f))
        {
            f.Seek(4, SeekOrigin.Begin);
            int count = br.ReadInt32();
            string baseName = Path.GetFileNameWithoutExtension(arcPath);
            f.Seek(0x20, SeekOrigin.Begin);
            for (int i = 0; i < count; i++)
            {
                uint offset      = br.ReadUInt32();
                uint unpackedSz  = br.ReadUInt32();
                uint packedSz    = br.ReadUInt32();
                br.ReadUInt32(); // padding
                bool isPacked = packedSz != 0;
                string raw = baseName + "#" + i.ToString("D5");
                entries.Add(new Entry
                {
                    Offset       = offset,
                    UnpackedSize = (int)unpackedSz,
                    PackedSize   = isPacked ? (int)packedSz : (int)unpackedSz,
                    Name         = raw,
                    RawName      = raw,
                });
            }
        }
        return entries;
    }

    static List<Entry> ParseDdp3(string arcPath)
    {
        // DDP3 index layout (derived from GARbro ArcDDP.cs + ArcHXP.cs):
        //   0x00  magic "DDP3"
        //   0x04  section_count : uint32
        //   0x20  section table: section_count * 8 bytes
        //         each section: blk_size(int32) + abs_offset(int32)
        //   at abs_offset: variable-length entry sequence (blk_size bytes):
        //         1-byte entry_size (>= 17), then:
        //           offset(4) + unpackedSz(4) + packedSz(4) + padding(4) + name_utf16le(entry_size-17)
        var entries = new List<Entry>();
        using (var f = new FileStream(arcPath, FileMode.Open, FileAccess.Read, FileShare.Read))
        using (var br = new BinaryReader(f))
        {
            f.Seek(4, SeekOrigin.Begin);
            int sectionCount = br.ReadInt32();
            f.Seek(0x20, SeekOrigin.Begin);

            var sections = new List<int[]>(); // [blkSize, absOffset]
            for (int i = 0; i < sectionCount; i++)
            {
                int blkSz  = br.ReadInt32();
                int absOff = br.ReadInt32();
                if (blkSz != 0)
                    sections.Add(new int[] { blkSz, absOff });
            }

            foreach (int[] sec in sections)
            {
                int blkSz  = sec[0];
                int absOff = sec[1];
                f.Seek(absOff, SeekOrigin.Begin);
                int remaining = blkSz;

                while (remaining > 0)
                {
                    int eSz = f.ReadByte();
                    if (eSz < 17) break;

                    byte[] buf = new byte[eSz - 1];
                    f.Read(buf, 0, eSz - 1);

                    long   eOffset  = BitConverter.ToUInt32(buf, 0);
                    int    eUnp     = (int)BitConverter.ToUInt32(buf, 4);
                    int    ePacked  = (int)BitConverter.ToUInt32(buf, 8);
                    // buf[12..15] = padding
                    // buf[16..eSz-2] = name in UTF-16 LE
                    string rawName = "";
                    int nameByteLen = eSz - 17;
                    if (nameByteLen > 0)
                    {
                        rawName = Encoding.Unicode.GetString(buf, 16, nameByteLen)
                                             .TrimEnd('\0');
                    }

                    string safeName = rawName;
                    foreach (char c in Path.GetInvalidFileNameChars())
                        safeName = safeName.Replace(c, '_');
                    if (safeName.Length == 0)
                        safeName = "entry" + entries.Count.ToString("D5");

                    bool isPacked = ePacked != 0;
                    entries.Add(new Entry
                    {
                        Offset       = eOffset,
                        UnpackedSize = eUnp,
                        PackedSize   = isPacked ? ePacked : eUnp,
                        Name         = safeName,
                        RawName      = rawName,
                    });

                    remaining -= eSz;
                }
            }
        }
        return entries;
    }

    // -----------------------------------------------------------------------
    // Extraction
    // -----------------------------------------------------------------------

    static int Main(string[] args)
    {
        if (args.Length < 2)
        {
            Console.Error.WriteLine("usage: extract_ddp <archive.dat> <out_dir>");
            return 1;
        }

        string arcPath = Path.GetFullPath(args[0]);
        string outDir  = Path.GetFullPath(args[1]);

        byte[] magic4 = new byte[4];
        using (var probe = new FileStream(arcPath, FileMode.Open, FileAccess.Read, FileShare.Read))
            probe.Read(magic4, 0, 4);

        bool isDdp3 = magic4[3] == (byte)'3';
        bool isDdp2 = magic4[3] == (byte)'2';

        if (!isDdp2 && !isDdp3)
        {
            Console.Error.WriteLine("ERROR: not a DDP2/DDP3 archive (magic: "
                + System.Text.Encoding.ASCII.GetString(magic4) + ")");
            return 3;
        }

        List<Entry> entries;
        try
        {
            entries = isDdp3 ? ParseDdp3(arcPath) : ParseDdp2(arcPath);
        }
        catch (Exception ex)
        {
            Console.Error.WriteLine("ERROR parsing index: " + ex.Message);
            return 4;
        }

        Directory.CreateDirectory(outDir);

        int ok = 0, skip = 0;
        using (var f = new FileStream(arcPath, FileMode.Open, FileAccess.Read, FileShare.Read))
        {
            foreach (Entry e in entries)
            {
                try
                {
                    f.Seek(e.Offset, SeekOrigin.Begin);
                    byte[] packed = new byte[e.PackedSize];
                    int n = f.Read(packed, 0, e.PackedSize);
                    if (n != e.PackedSize)
                        throw new EndOfStreamException("short read at offset 0x" + e.Offset.ToString("X"));

                    bool isPacked = e.PackedSize != e.UnpackedSize;
                    byte[] raw;
                    if (isPacked)
                    {
                        raw = new byte[e.UnpackedSize];
                        new ShsReader(packed).Unpack(raw);
                    }
                    else
                    {
                        raw = packed;
                    }

                    // Only save BMP images (first 2 bytes = "BM")
                    if (raw.Length < 4 || raw[0] != 0x42 || raw[1] != 0x4D)
                    {
                        skip++;
                        continue;
                    }

                    // Decode BMP to get pixel dimensions
                    BitmapSource src;
                    using (var ms = new MemoryStream(raw))
                    {
                        var dec = new BmpBitmapDecoder(ms,
                            BitmapCreateOptions.PreservePixelFormat,
                            BitmapCacheOption.OnLoad);
                        src = dec.Frames[0];
                    }

                    string outPath = Path.Combine(outDir, e.Name + ".bmp");
                    var enc = new BmpBitmapEncoder();
                    enc.Frames.Add(BitmapFrame.Create(src));
                    using (var fs = File.Create(outPath))
                        enc.Save(fs);

                    Console.WriteLine(e.RawName + "\t" + src.PixelWidth + "\t" + src.PixelHeight);
                    ok++;
                }
                catch (Exception ex)
                {
                    string msg = ex.InnerException != null ? ex.InnerException.Message : ex.Message;
                    Console.Error.WriteLine("SKIP " + e.RawName + ": " + msg);
                    skip++;
                }
            }
        }

        Console.Error.WriteLine("Done: " + ok + " extracted, " + skip + " skipped.");
        return 0;
    }
}
