# Inno Setup writes the installer's version strings into fixed-width,
# space-padded slots ("PSDGlance" + 52 spaces). SignPath matches product name
# and version exactly, so trailing spaces make signing fail. This trims them
# in place: each value is NUL-filled to its old width and only its
# wValueLength changes, so no byte moves (Inno's loader relies on fixed file
# offsets). The PE checksum is then recomputed.
param([Parameter(Mandatory)][string]$Path)

Add-Type -TypeDefinition @"
using System;
using System.IO;
using System.Text;

public static class VersionStringTrimmer {
    static byte[] b;
    static int U16(int o) { return b[o] | (b[o + 1] << 8); }
    static int I32(int o) { return BitConverter.ToInt32(b, o); }
    static void W16(int o, int v) { b[o] = (byte)v; b[o + 1] = (byte)(v >> 8); }
    static int Align4(int start, int pos) { return start + ((pos - start + 3) & ~3); }

    public static int Run(string path) {
        b = File.ReadAllBytes(path);
        int pe = I32(0x3C);
        if (Encoding.ASCII.GetString(b, pe, 4) != "PE\0\0") throw new Exception("not a PE file");
        int sections = U16(pe + 6), optSize = U16(pe + 20), opt = pe + 24;
        bool pe32plus = U16(opt) == 0x20B;
        int rsrcRva = I32(opt + (pe32plus ? 112 : 96) + 2 * 8);
        int secTable = opt + optSize;
        Func<int, int> rvaToOff = rva => {
            for (int i = 0; i < sections; i++) {
                int s = secTable + 40 * i, va = I32(s + 12), size = Math.Max(I32(s + 8), I32(s + 16)), raw = I32(s + 20);
                if (rva >= va && rva < va + size) return rva - va + raw;
            }
            throw new Exception("RVA outside sections");
        };
        int rsrc = rvaToOff(rsrcRva);

        int changed = 0;
        // Resource tree: type RT_VERSION (16) -> any name -> every language.
        foreach (int nameDir in Children(rsrc, rsrc, 16))
            foreach (int langDir in Children(rsrc, nameDir, -1))
                foreach (int dataEntry in Children(rsrc, langDir, -1)) {
                    int start = rvaToOff(I32(dataEntry)), size = I32(dataEntry + 4);
                    changed += Walk(start, start, start + size, 0, null);
                }

        int checksumAt = opt + 64;
        if (changed > 0 && I32(checksumAt) != 0) {
            Array.Clear(b, checksumAt, 4);
            ulong sum = 0;
            for (int i = 0; i + 1 < b.Length; i += 2) { sum += (ulong)U16(i); sum = (sum & 0xFFFF) + (sum >> 16); }
            if ((b.Length & 1) != 0) { sum += b[b.Length - 1]; sum = (sum & 0xFFFF) + (sum >> 16); }
            sum = (sum & 0xFFFF) + (sum >> 16);
            uint checksum = (uint)(sum + (ulong)b.Length);
            BitConverter.GetBytes(checksum).CopyTo(b, checksumAt);
        }
        if (changed > 0) File.WriteAllBytes(path, b);
        return changed;
    }

    // Entries of a resource directory; id < 0 means all. Returns the entry's
    // target: a subdirectory offset, or a data entry offset at the leaf level.
    static System.Collections.Generic.IEnumerable<int> Children(int rsrc, int dir, int id) {
        int count = U16(dir + 12) + U16(dir + 14);
        for (int i = 0; i < count; i++) {
            int e = dir + 16 + 8 * i, name = I32(e), target = I32(e + 4);
            if (id >= 0 && ((name & 0x80000000) != 0 || name != id)) continue;
            yield return rsrc + (target & 0x7FFFFFFF);
        }
    }

    // Walks a version-info block; trims String values under StringFileInfo.
    static int Walk(int root, int o, int end, int depth, string parentKey) {
        int len = U16(o), valueLen = U16(o + 2);
        if (len < 6 || o + len > end) return 0;
        int k = o + 6; var key = new StringBuilder();
        while (k + 1 < o + len && U16(k) != 0) { key.Append((char)U16(k)); k += 2; }
        int value = Align4(root, k + 2);
        int changed = 0;
        if (depth == 3) {  // VS_VERSIONINFO > StringFileInfo > StringTable > String
            int chars = valueLen;
            int n = 0;
            while (n < chars && U16(value + 2 * n) != 0) n++;
            int trimmed = n;
            while (trimmed > 0 && U16(value + 2 * (trimmed - 1)) == ' ') trimmed--;
            if (trimmed < n) {
                for (int i = trimmed; i < chars; i++) W16(value + 2 * i, 0);
                W16(o + 2, trimmed + 1);
                changed++;
            }
            return changed;
        }
        int valueBytes = depth == 0 ? valueLen : (parentKey == "VarFileInfo" ? valueLen : valueLen * 2);
        int child = Align4(root, value + valueBytes);
        string myKey = key.ToString();
        if (depth == 0 || (depth == 1 && myKey == "StringFileInfo") || depth == 2) {
            while (child + 6 <= o + len) {
                int clen = U16(child);
                if (clen == 0) break;
                changed += Walk(root, child, o + len, depth + 1, myKey);
                child = Align4(root, child + clen);
            }
        }
        return changed;
    }
}
"@

$n = [VersionStringTrimmer]::Run((Resolve-Path $Path).Path)
"trimmed $n padded version strings in $(Split-Path $Path -Leaf)"
