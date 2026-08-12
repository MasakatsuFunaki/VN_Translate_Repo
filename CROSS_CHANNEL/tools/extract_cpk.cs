using System;
using System.Collections;
using System.IO;
using System.Reflection;
using System.Windows.Media.Imaging;

/// <summary>
/// Extract all image entries from a WillPlus CPK archive using GARbro DLLs.
/// Usage: extract_cpk.exe &lt;archive.cpk&gt; &lt;out_dir&gt;
/// Prints one line per extracted image: &lt;entry_name&gt; TAB &lt;width&gt; TAB &lt;height&gt;
/// Requires GARbro DLLs (GameRes.dll, ArcFormats.dll, ArcLegacy.dll) in the same directory.
/// </summary>
class ExtractCpk
{
    static int Main(string[] args)
    {
        if (args.Length < 2)
        {
            Console.Error.WriteLine("usage: extract_cpk <archive.cpk> <out_dir>");
            return 1;
        }

        string arcPath = Path.GetFullPath(args[0]);
        string outDir  = Path.GetFullPath(args[1]);

        string baseDir = AppDomain.CurrentDomain.BaseDirectory;
        AppDomain.CurrentDomain.AssemblyResolve += (s, e) =>
        {
            var name = new AssemblyName(e.Name).Name;
            var p = Path.Combine(baseDir, name + ".dll");
            if (File.Exists(p)) return Assembly.LoadFrom(p);
            return null;
        };

        // Load format DLLs — this populates GARbro's FormatCatalog with all supported formats,
        // including WillPlus CPK/HG3.
        Assembly.LoadFrom(Path.Combine(baseDir, "ArcFormats.dll"));
        Assembly.LoadFrom(Path.Combine(baseDir, "ArcLegacy.dll"));
        var gr = Assembly.LoadFrom(Path.Combine(baseDir, "GameRes.dll"));

        var arcFileType = gr.GetType("GameRes.ArcFile");
        var tryOpenMethod = arcFileType.GetMethod("TryOpen", new[] { typeof(string) });

        object arc = null;
        try
        {
            arc = tryOpenMethod.Invoke(null, new object[] { arcPath });
        }
        catch (Exception ex)
        {
            Console.Error.WriteLine("ERROR opening archive: " + (ex.InnerException != null ? ex.InnerException.Message : ex.Message));
            return 2;
        }

        if (arc == null)
        {
            Console.Error.WriteLine("ERROR: Unsupported archive format: " + arcPath);
            return 2;
        }

        Directory.CreateDirectory(outDir);

        var dirProp        = arcFileType.GetProperty("Dir");
        var openImageMethod = arcFileType.GetMethod("OpenImage");
        var entries        = (IEnumerable)dirProp.GetValue(arc);

        int ok = 0, skip = 0;
        foreach (var entry in entries)
        {
            var typePropInst = entry.GetType().GetProperty("Type");
            var namePropInst = entry.GetType().GetProperty("Name");
            var entryType = typePropInst != null ? (typePropInst.GetValue(entry) ?? "").ToString() : "";
            var entryName = namePropInst != null ? (namePropInst.GetValue(entry) ?? "").ToString() : ("entry" + ok.ToString("D5"));

            // CPK entries have empty Type until the entry is actually opened.
            // Try OpenImage on all entries; non-image entries will throw and be skipped.

            try
            {
                object decoder = openImageMethod.Invoke(arc, new object[] { entry });
                if (decoder == null) { skip++; continue; }

                var disposeDecoder = decoder as IDisposable;
                try
                {
                    var imageProp = decoder.GetType().GetProperty("Image");
                    if (imageProp == null) { skip++; continue; }

                    object imgData = imageProp.GetValue(decoder);
                    var bitmapProp = imgData.GetType().GetProperty("Bitmap");
                    BitmapSource bitmap = (BitmapSource)bitmapProp.GetValue(imgData);

                    string safeName = entryName;
                    foreach (char c in Path.GetInvalidFileNameChars())
                        safeName = safeName.Replace(c, '_');
                    if (string.IsNullOrEmpty(safeName))
                        safeName = "entry" + ok.ToString("D5");

                    string outPath = Path.Combine(outDir, safeName + ".bmp");
                    var enc = new BmpBitmapEncoder();
                    enc.Frames.Add(BitmapFrame.Create(bitmap));
                    using (var fs = File.Create(outPath))
                        enc.Save(fs);

                    Console.WriteLine(entryName + "\t" + bitmap.PixelWidth + "\t" + bitmap.PixelHeight);
                    ok++;
                }
                finally
                {
                    if (disposeDecoder != null) { disposeDecoder.Dispose(); }
                }
            }
            catch (Exception ex)
            {
                string msg = ex.InnerException != null ? ex.InnerException.Message : ex.Message;
                Console.Error.WriteLine("SKIP " + entryName + ": " + msg);
                skip++;
            }
        }

        ((IDisposable)arc).Dispose();
        Console.Error.WriteLine("Done: " + ok + " extracted, " + skip + " skipped.");
        return 0;
    }
}
