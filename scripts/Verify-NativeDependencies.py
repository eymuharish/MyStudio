"""Read native PE imports without executing images or depending on installed CRT."""
import argparse, json, os, struct
from pathlib import Path

def imports(path):
    data=path.read_bytes()
    if data[:2]!=b'MZ': raise ValueError('Not a PE image: '+str(path))
    pe=struct.unpack_from('<I',data,0x3c)[0]
    if data[pe:pe+4]!=b'PE\0\0': raise ValueError('Invalid PE signature')
    sections=struct.unpack_from('<H',data,pe+6)[0]
    optional_size=struct.unpack_from('<H',data,pe+20)[0]
    optional=pe+24
    magic=struct.unpack_from('<H',data,optional)[0]
    directory=optional+(112 if magic==0x20b else 96)
    import_rva=struct.unpack_from('<I',data,directory+8)[0]
    section_start=optional+optional_size
    def offset(rva):
        for n in range(sections):
            base=section_start+n*40
            size,virtual,raw_size,raw=struct.unpack_from('<IIII',data,base+8)
            if virtual<=rva<virtual+max(size,raw_size): return raw+rva-virtual
        raise ValueError('RVA outside image')
    names=[]
    if import_rva:
        cursor=offset(import_rva)
        while any(struct.unpack_from('<IIIII',data,cursor)):
            name=offset(struct.unpack_from('<I',data,cursor+12)[0])
            names.append(data[name:data.index(0,name)].decode('ascii').lower())
            cursor+=20
    delay_rva=struct.unpack_from('<I',data,directory+13*8)[0]
    if delay_rva:
        cursor=offset(delay_rva)
        image_base=struct.unpack_from('<Q' if magic==0x20b else '<I',data,optional+(24 if magic==0x20b else 28))[0]
        while any(struct.unpack_from('<IIIIIIII',data,cursor)):
            attributes,name_rva=struct.unpack_from('<II',data,cursor)
            name=offset(name_rva if attributes&1 else name_rva-image_base)
            names.append(data[name:data.index(0,name)].decode('ascii').lower())
            cursor+=32
    return sorted(set(names))

def main():
    parser=argparse.ArgumentParser(); parser.add_argument('engine',type=Path); parser.add_argument('--output',type=Path); parser.add_argument('--portable',action='store_true')
    args=parser.parse_args(); files=list(args.engine.rglob('*.dll'))+list(args.engine.rglob('*.exe'))
    system=Path(os.environ['SystemRoot'])/'System32'
    local={}
    for path in files: local.setdefault(path.parent.resolve(),set()).add(path.name.lower())
    if args.portable:
        if {p.name for p in args.engine.iterdir()}!={'MyStudio.exe','appfiles'}: raise SystemExit('Portable root must contain only MyStudio.exe and appfiles')
        payload=args.engine/'appfiles'
        config=json.loads((payload/'MyStudio.runtimeconfig.json').read_text(encoding='utf-8-sig'))['runtimeOptions']
        if not config.get('includedFrameworks') or config.get('framework') or config.get('frameworks'): raise SystemExit('Installed .NET framework dependency')
        for name in ('coreclr.dll','hostfxr.dll','hostpolicy.dll','Microsoft.UI.Xaml.dll','Microsoft.WindowsAppRuntime.dll'):
            if not (payload/name).is_file(): raise SystemExit('Missing runtime: '+name)
        launcher_imports=imports(args.engine/'MyStudio.exe')
        if any(n.startswith(('msvcp','vcruntime','concrt','vccorlib','msvcr','ucrtbase','api-ms-win-crt')) for n in launcher_imports): raise SystemExit('Launcher requires external C/C++ runtime')
    results=[]; missing=[]
    for path in files:
        names=imports(path)
        # Plugins resolve their engine's adjacent dependencies; do not mistake a
        # CRT in a different application/engine directory for a usable local DLL.
        search=[path.parent.resolve()]
        if path.parent.name.lower()=='plugins': search.append(path.parent.parent.resolve())
        if path.parent.name.lower() in ('x64','amd64'): search.append(path.parent.parent.resolve())
        available=set().union(*(local.get(folder,set()) for folder in search))
        absent=[n for n in names if n not in available and not n.startswith(('api-ms-win-','ext-ms-win-')) and
                ((n!='msvcrt.dll' and n.startswith(('msvcp','vcruntime','concrt','vccorlib','msvcr','avcodec','avutil','swscale','libx','libssl','libcrypto','hostfxr','hostpolicy','coreclr'))) or not (system/n).is_file())]
        results.append({'path':str(path.relative_to(args.engine)),'imports':names,'missing':absent})
        missing.extend(absent)
    result={'result':'FAIL' if missing else 'PASS','images':results}
    if args.output: args.output.write_text(json.dumps(result,indent=2),encoding='utf-8')
    print('Native PE dependency closure '+result['result']+' images='+str(len(files)))
    if missing: raise SystemExit('Missing imports: '+', '.join(sorted(set(missing))))
if __name__=='__main__': main()
