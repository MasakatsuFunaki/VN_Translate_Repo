using System;
using System.IO;
using System.Reflection;
using System.Windows.Media.Imaging;

class ExtractCbg
{
    static int Main(string[] args)
    {
        if (args.Length < 2)
        {
            Console.Error.WriteLine("usage: extract_cbg <in.cbg> <out.bmp>");
            return 1;
        }
        try
        {
            string baseDir = AppDomain.CurrentDomain.BaseDirectory;
            AppDomain.CurrentDomain.AssemblyResolve += (s, e) =>
            {
                string name = new AssemblyName(e.Name).Name;
                string p = Path.Combine(baseDir, name + ".dll");
                if (File.Exists(p)) return Assembly.LoadFrom(p);
                return null;
            };

            var arcFmt = Assembly.LoadFrom(Path.Combine(baseDir, "ArcFormats.dll"));
            var gameRes = Assembly.LoadFrom(Path.Combine(baseDir, "GameRes.dll"));

            Type cbgType = null;
            foreach (var t in arcFmt.GetTypes())
                if (t.Name == "CompressedBGFormat") { cbgType = t; break; }

            object cbgInst = Activator.CreateInstance(cbgType);

            byte[] inputBytes = File.ReadAllBytes(args[0]);
            Type bs = gameRes.GetType("GameRes.BinaryStream");
            var fromArray = bs.GetMethod("FromArray", new[] { typeof(byte[]), typeof(string) });

            object stream = fromArray.Invoke(null, new object[] { inputBytes, args[0] });
            object meta = cbgType.GetMethod("ReadMetaData").Invoke(cbgInst, new[] { stream });
            uint w = (uint)meta.GetType().GetProperty("Width").GetValue(meta, null);
            uint h = (uint)meta.GetType().GetProperty("Height").GetValue(meta, null);
            int bpp = (int)meta.GetType().GetProperty("BPP").GetValue(meta, null);
            Console.WriteLine(string.Format("  {0}x{1} {2}bpp", w, h, bpp));

            stream = fromArray.Invoke(null, new object[] { inputBytes, args[0] });
            object imageData = cbgType.GetMethod("Read").Invoke(cbgInst, new[] { stream, meta });

            BitmapSource src = (BitmapSource)imageData.GetType().GetProperty("Bitmap").GetValue(imageData, null);

            var enc = new BmpBitmapEncoder();
            enc.Frames.Add(BitmapFrame.Create(src));
            using (var fs = File.Create(args[1])) enc.Save(fs);
            Console.WriteLine(string.Format("  -> {0}", args[1]));
            return 0;
        }
        catch (Exception ex)
        {
            Console.Error.WriteLine("ERROR: " + ex.GetType().Name + ": " + ex.Message);
            Console.Error.WriteLine(ex.StackTrace);
            if (ex.InnerException != null) Console.Error.WriteLine("Inner: " + ex.InnerException.Message);
            return 99;
        }
    }
}
