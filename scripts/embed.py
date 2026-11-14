from pathlib import Path
import gzip
import json
import re
import sys
from generate_locales import generate
from generate_pixel_layout import generate as generate_layout
generate_layout()
generate()
root=Path(__file__).resolve().parents[1]
windows=json.loads((root/'resources/timezones/windows-zones.json').read_text('utf-8'))
assert len(windows['zones'])>=140
zone_script='// Generated from Microsoft Windows timezone data.\nwindow.MatrixTimezones='+json.dumps(windows['rules'],separators=(',',':'))+';\n'
zone_script+='window.MatrixWindowsZones='+json.dumps(windows,separators=(',',':'),ensure_ascii=False)+';\n'
zone_target=root/'web/timezones.js'
if not zone_target.exists() or zone_target.read_text('utf-8')!=zone_script:zone_target.write_text(zone_script,encoding='utf-8')
dest=Path(sys.argv[1]) if len(sys.argv)>1 else root/'main/assets'
board=sys.argv[2] if len(sys.argv)>2 else ''
dest.mkdir(parents=True,exist_ok=True)
for name in ['index.html','app.js','style.css','pixel-art.js','locales.js','i18n.js','forms.js','timezones.js','timezone-picker.js','printer-manager.js','online-update.js']:
 source=(root/'web'/name).read_text('utf-8')
 if board=='clockwise-esp32-hub75':
  if name=='index.html':
   source=re.sub(r'<!-- external-display -->.*?<!-- /external-display -->','',source,flags=re.S)
   source=source.replace('<div class="pixel-controls wide">','<div class="pixel-controls wide"><div class="rotation-control"><button type="button" id="pixel-turn" data-zh="旋转 90°" data-en="Rotate 90°"></button><output id="pixel-angle-value" for="pixel-angle">0°</output><input id="pixel-angle" type="hidden" value="0"></div>')
   source=source.replace('<html lang=', '<html data-display="hub75" lang=')
   source=source.replace('iDotMatrix × Bambu Lab','3D Print Pixel Monitor').replace('iDotMatrix <span>× Bambu Lab</span>','3D Print <span>Pixel Monitor</span>')
   source=source.replace('像素屏设置" data-en="Pixel display settings','板载像素屏设置" data-en="Onboard display settings')
   source=source.replace('待机使用像素屏内置时钟，自行走时。','板载屏使用联网校时。').replace('The built-in idle clock keeps time independently.','The onboard display uses network time.')
  elif name=='app.js':
   source=re.sub(r'/\* external-display \*/.*?/\* /external-display \*/','',source,flags=re.S)
 p=dest/(name.replace('.','_')+'.gz')
 data=gzip.compress(source.encode('utf-8'),mtime=0)
 if not p.exists() or p.read_bytes()!=data:p.write_bytes(data)
