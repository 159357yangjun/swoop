import struct, os

os.makedirs("src/gui", exist_ok=True)

W = H = 32
pix = bytearray()
for y in range(H):
    for x in range(W):
        edge = (x < 2 or y < 2 or x >= W - 2 or y >= H - 2)
        if edge:
            r, g, b, a = 0xFF, 0xFF, 0xFF, 0xFF
        else:
            r, g, b, a = 0x25, 0x63, 0xEB, 0xFF
        pix += bytes((b, g, r, a))

rows = [pix[y * W * 4:(y + 1) * W * 4] for y in range(H)]
pix = b"".join(reversed(rows))

bih = struct.pack("<IiiHHIIiiII", 40, W, H * 2, 1, 32, 0, len(pix), 0, 0, 0, 0)
andmask = b"\x00" * (H * ((W + 31) // 8))
icondir = struct.pack("<HHH", 0, 1, 1)
entry = struct.pack("<BBBBHHII", W, H, 0, 0, 1, 32,
                    len(bih) + len(pix) + len(andmask), 22)
with open("src/gui/app.ico", "wb") as f:
    f.write(icondir + entry + bih + pix + andmask)

manifest = """<?xml version="1.0" encoding="UTF-8" standalone="yes"?>
<assembly xmlns="urn:schemas-microsoft-com:asm.v1" manifestVersion="1.0">
  <dependency><dependentAssembly>
    <assemblyIdentity type="win32" name="Microsoft.Windows.Common-Controls" version="6.0.0.0" processorArchitecture="*" publicKeyToken="6595b64144ccf1df" language="*"/>
  </dependentAssembly></dependency>
  <application xmlns="urn:schemas-microsoft-com:asm.v3">
    <windowsSettings>
      <dpiAware xmlns="http://schemas.microsoft.com/SMI/2005/WindowsSettings">true/pm</dpiAware>
      <dpiAwareness xmlns="http://schemas.microsoft.com/SMI/2016/WindowsSettings">permonitorv2,permonitor</dpiAwareness>
    </windowsSettings>
  </application>
</assembly>
"""
with open("src/gui/app.manifest", "w") as f:
    f.write(manifest)

print("assets written: app.ico, app.manifest")
