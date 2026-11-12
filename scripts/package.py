"""Package a built Windows plugin without dependencies or local planning files."""
import argparse
import hashlib
import pathlib
import zipfile

root = pathlib.Path(__file__).resolve().parents[1]
p = argparse.ArgumentParser()
p.add_argument('--dll', type=pathlib.Path, default=root/'build-win/Analyzers/Release/SpdifAnalyzer.dll')
p.add_argument('--output', type=pathlib.Path, default=root/'artifacts/SpdifAnalyzer-windows-x64.zip')
a = p.parse_args()
if not a.dll.is_file():
    p.error('Build the Release DLL first')
a.output.parent.mkdir(parents=True, exist_ok=True)
files = [(a.dll, 'SpdifAnalyzer.dll'), (root/'README.md', 'README.md')]
files += [(f, 'docs/'+f.name) for f in sorted((root/'docs').glob('*.md'))]
manifest = ''.join(hashlib.sha256(f.read_bytes()).hexdigest()+'  '+name+'\n' for f, name in files)
with zipfile.ZipFile(a.output, 'w', compression=zipfile.ZIP_DEFLATED) as z:
    for f, name in files:
        z.write(f, name)
    z.writestr('SHA256SUMS.txt', manifest)
a.output.with_suffix(a.output.suffix+'.sha256').write_text(hashlib.sha256(a.output.read_bytes()).hexdigest()+'  '+a.output.name+'\n')
print(a.output)
