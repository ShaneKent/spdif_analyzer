"""Create/verify production analyzer packages; no GitHub or installation writes.

The legacy --dll/--output Windows invocation remains supported. Release packages
attest the checked-out CI source, not an embedded or signed binary identity.
"""
import argparse
import hashlib
import json
import os
from pathlib import Path
import re
import struct
import subprocess
import tempfile
import zipfile

ROOT = Path(__file__).resolve().parents[1]
MIN_LOGIC = '2.3.43'
DOCS = ('CODECS.md', 'FEATURES.md', 'INSTALL.md', 'OFFLINE.md', 'PERFORMANCE.md',
        'RELEASING.md', 'REPORT.md', 'RESULTS.md', 'VALIDATION.md')
PRODUCTION_NAMES = {'windows': ('SpdifAnalyzer.dll',),
                    'linux': ('libSpdifAnalyzer.so', 'SpdifAnalyzer.so'),
                    'macos': ('libSpdifAnalyzer.so', 'SpdifAnalyzer.so',
                              'libSpdifAnalyzer.dylib', 'SpdifAnalyzer.dylib')}
MAX_ARCHIVE_BYTES = 128 * 1024 * 1024


def sha(data):
    return hashlib.sha256(data).hexdigest()


def binary_identity(data):
    """Inspect actual object headers, independent of runner names/file suffixes."""
    if len(data) < 32:
        raise ValueError('Truncated analyzer binary')
    if data[:2] == b'MZ':
        if len(data) < 64:
            raise ValueError('Truncated PE header')
        pos = struct.unpack_from('<I', data, 60)[0]
        if pos > len(data) - 24 or data[pos:pos+4] != b'PE\0\0':
            raise ValueError('Invalid PE signature')
        machine, sections = struct.unpack_from('<HH', data, pos+4)
        characteristics = struct.unpack_from('<H', data, pos+22)[0]
        if not sections or not characteristics & 0x2000:
            raise ValueError('PE input must be a DLL')
        arch = {0x8664: 'x64', 0xAA64: 'arm64'}.get(machine)
        platform = 'windows'
    elif data[:4] == b'\x7fELF':
        if data[4] != 2 or data[5] not in (1, 2) or len(data) < 64:
            raise ValueError('Analyzer must be a 64-bit ELF shared object')
        endian = '<' if data[5] == 1 else '>'
        kind, machine = struct.unpack_from(endian+'HH', data, 16)
        if kind != 3:
            raise ValueError('ELF input must be a shared object')
        arch = {62: 'x64', 183: 'arm64'}.get(machine)
        platform = 'linux'
    elif data[:4] in (b'\xcf\xfa\xed\xfe', b'\xfe\xed\xfa\xcf'):
        endian = '<' if data[:4] == b'\xcf\xfa\xed\xfe' else '>'
        cpu, _, kind = struct.unpack_from(endian+'III', data, 4)
        if kind not in (6, 8):
            raise ValueError('Mach-O input must be a dylib or bundle')
        arch = {0x01000007: 'x64', 0x0100000C: 'arm64'}.get(cpu)
        platform = 'macos'
    elif data[:4] in (b'\xca\xfe\xba\xbe', b'\xbe\xba\xfe\xca'):
        endian = '>' if data[:4] == b'\xca\xfe\xba\xbe' else '<'
        count = struct.unpack_from(endian+'I', data, 4)[0]
        if count != 2 or len(data) < 8+20*count:
            raise ValueError('Only a two-architecture universal2 Mach-O is supported')
        arches = set()
        for index in range(count):
            cpu, _, offset, size, _ = struct.unpack_from(endian+'IIIII', data, 8+20*index)
            if offset < 8+20*count or not size or offset > len(data)-size:
                raise ValueError('Invalid fat Mach-O extent')
            if data[offset:offset+4] not in (b'\xcf\xfa\xed\xfe', b'\xfe\xed\xfa\xcf'):
                raise ValueError('Universal slices must be thin 64-bit Mach-O objects')
            platform, arch = binary_identity(data[offset:offset+size])
            if platform != 'macos' or cpu != {'x64': 0x01000007, 'arm64': 0x0100000C}.get(arch):
                raise ValueError('Inconsistent fat Mach-O architecture')
            arches.add(arch)
        if arches != {'x64', 'arm64'}:
            raise ValueError('Universal2 requires x64 and arm64 slices')
        return 'macos', 'universal2'
    else:
        raise ValueError('Unsupported analyzer object format')
    if arch is None:
        raise ValueError('Unsupported analyzer architecture')
    return platform, arch


def git(root, *args):
    return subprocess.check_output(['git', '-C', str(root), *args], text=True).strip()


def source_identity(root, release, tag, expected_source):
    version = re.search(r'^#define SPDIF_ANALYZER_VERSION "([0-9]+\.[0-9]+\.[0-9]+)"$',
                        (root/'src/core/Version.h').read_text(), re.M)
    sdk = re.search(r'GIT_REPOSITORY https://github.com/saleae/AnalyzerSDK.git GIT_TAG ([0-9a-f]{40})',
                    (root/'CMakeLists.txt').read_text())
    if not version or not sdk:
        raise ValueError('Cannot determine centralized version or pinned SDK revision')
    commit = git(root, 'rev-parse', 'HEAD')
    dirty = bool(git(root, 'status', '--porcelain', '--untracked-files=no'))
    if not re.fullmatch('[0-9a-f]{40}', commit) or (expected_source and commit != expected_source):
        raise ValueError('Source commit does not match requested provenance')
    version = version[1]
    if tag and tag != 'v'+version:
        raise ValueError('Tag must match centralized version v'+version)
    if release:
        if dirty or not tag:
            raise ValueError('Release packaging requires a clean tracked checkout and matching tag')
        if git(root, 'rev-parse', 'refs/tags/'+tag+'^{commit}') != commit:
            raise ValueError('Release tag does not identify the checked-out source')
        git(root, 'ls-files', '--error-unmatch', 'README.md', 'src/core/Version.h',
            'CMakeLists.txt', *('docs/'+name for name in DOCS))
    return dict(version=version, source_sha=commit, sdk_revision=sdk[1],
                source_dirty=dirty, release_qualified=release, tag=tag or None)


def metadata(platform, arch, plugin_name, binary, source):
    return dict(schema_version=1, analyzer='S/PDIF Analyzer', **source,
                platform=platform, architecture=arch, plugin=plugin_name,
                plugin_sha256=sha(binary), minimum_logic_version=MIN_LOGIC,
                provenance='Checked-out source/build attestation; not embedded binary provenance or code signing',
                signing='unsigned; not notarized',
                runtime_requirements={
                    'windows': '64-bit Logic; Microsoft Visual C++ runtime supplied by the host/installed separately if needed',
                    'linux': '64-bit Logic and compatible system C++/glibc runtime; release CI uses Ubuntu 22.04; local builds may differ',
                    'macos': 'Logic process architecture must match this binary; system C++ runtime; see release build OS'}[platform],
                sdk_runtime='Provided by Logic; never install an SDK runtime replacement',
                gui_loading_validation='CI builds/tests do not establish GUI loading on every platform; see docs/VALIDATION.md')


def zip_bytes(entries):
    # Stable ordering, timestamps, permissions, and JSON make identical inputs
    # produce identical archives within a fixed Python/zlib toolchain.
    import io
    stream = io.BytesIO()
    with zipfile.ZipFile(stream, 'w', compression=zipfile.ZIP_DEFLATED) as archive:
        for name, data in sorted(entries.items()):
            info = zipfile.ZipInfo(name, date_time=(1980, 1, 1, 0, 0, 0))
            info.create_system = 3
            info.external_attr = (0o100644 << 16)
            info.compress_type = zipfile.ZIP_DEFLATED
            archive.writestr(info, data)
    return stream.getvalue()


def verify(path, *, expected_source=None, expected_tag=None, require_release=False):
    path = Path(path)
    if path.stat().st_size > MAX_ARCHIVE_BYTES:
        raise ValueError('Archive exceeds package size limit')
    checksum = path.with_suffix(path.suffix+'.sha256')
    if checksum.read_text() != sha(path.read_bytes())+'  '+path.name+'\n':
        raise ValueError('External archive checksum does not match')
    with zipfile.ZipFile(path) as archive:
        infos = archive.infolist()
        if len(infos) != len({i.filename for i in infos}) or sum(i.file_size for i in infos) > MAX_ARCHIVE_BYTES:
            raise ValueError('Duplicate members or excessive uncompressed package size')
        for info in infos:
            if info.is_dir() or info.external_attr >> 16 & 0o170000 == 0o120000:
                raise ValueError('Directories and symlinks are forbidden')
        entries = {i.filename: archive.read(i) for i in infos}
    meta = json.loads(entries['PACKAGE.json'])
    if not isinstance(meta, dict) or not isinstance(meta.get('plugin'), str):
        raise ValueError('Invalid package metadata object')
    strings = ('version', 'source_sha', 'sdk_revision', 'platform', 'architecture',
               'plugin_sha256', 'minimum_logic_version')
    if not all(isinstance(meta.get(key), str) for key in strings) or not all(
            type(meta.get(key)) is bool for key in ('source_dirty', 'release_qualified')):
        raise ValueError('Invalid typed provenance fields')
    if meta.get('tag') is not None and meta['tag'] != 'v'+meta['version']:
        raise ValueError('Inconsistent package version/tag')
    platform, arch = binary_identity(entries[meta['plugin']])
    allowed = {'README.md', 'PACKAGE.json', 'SHA256SUMS.txt', meta['plugin']} | {'docs/'+name for name in DOCS}
    if set(entries) != allowed or meta['plugin'] not in PRODUCTION_NAMES.get(platform, ()):
        raise ValueError('Package must contain exactly the production analyzer and approved public documents')
    if meta.get('schema_version') != 1 or (meta['platform'], meta['architecture']) != (platform, arch):
        raise ValueError('Object architecture does not match package metadata')
    if meta['plugin_sha256'] != sha(entries[meta['plugin']]) or meta['minimum_logic_version'] != MIN_LOGIC:
        raise ValueError('Plugin hash/minimum Logic metadata does not match')
    if not re.fullmatch(r'[0-9]+\.[0-9]+\.[0-9]+', meta['version']) or not all(re.fullmatch('[0-9a-f]{40}', meta[k]) for k in ('source_sha', 'sdk_revision')):
        raise ValueError('Invalid version/source/SDK provenance')
    if expected_source and meta['source_sha'] != expected_source:
        raise ValueError('Unexpected source commit')
    if expected_tag and (meta['tag'] != expected_tag or expected_tag != 'v'+meta['version']):
        raise ValueError('Unexpected version/tag')
    if require_release and (not meta['release_qualified'] or meta['source_dirty'] or meta['tag'] != 'v'+meta['version']):
        raise ValueError('Package is not qualified for a tagged release')
    manifest = ''.join(sha(data)+'  '+name+'\n' for name, data in sorted(entries.items()) if name != 'SHA256SUMS.txt')
    if entries['SHA256SUMS.txt'].decode('ascii') != manifest:
        raise ValueError('Internal member checksums do not match')
    return meta


def package(plugin, output=None, *, root=ROOT, release=False, tag=None, expected_source=None):
    plugin = Path(plugin)
    if plugin.is_symlink() or not plugin.is_file() or plugin.stat().st_size > MAX_ARCHIVE_BYTES:
        raise ValueError('Build the production analyzer first; symlinks and oversized binaries are refused')
    binary = plugin.read_bytes()
    platform, arch = binary_identity(binary)
    if plugin.name not in PRODUCTION_NAMES[platform]:
        raise ValueError('Only the production SpdifAnalyzer binary may be packaged')
    source = source_identity(root, release, tag, expected_source)
    meta = metadata(platform, arch, plugin.name, binary, source)
    if output is None:
        output = root/'artifacts'/('SpdifAnalyzer-'+source['version']+'-'+platform+'-'+arch+'.zip')
    output = Path(output)
    checksum = output.with_suffix(output.suffix+'.sha256')
    if output.exists() or checksum.exists():
        raise FileExistsError('Refusing to overwrite an existing archive or checksum')
    if (root/'README.md').is_symlink():
        raise ValueError('Public documentation must be a regular file')
    entries = {plugin.name: binary, 'README.md': (root/'README.md').read_bytes(),
               'PACKAGE.json': (json.dumps(meta, indent=2, sort_keys=True)+'\n').encode()}
    for name in DOCS:
        document = root/'docs'/name
        if document.is_symlink():
            raise ValueError('Public documentation must be a regular file')
        entries['docs/'+name] = document.read_bytes()
    if sum(map(len, entries.values())) > MAX_ARCHIVE_BYTES:
        raise ValueError('Package content exceeds size limit')
    entries['SHA256SUMS.txt'] = ''.join(sha(data)+'  '+name+'\n' for name, data in sorted(entries.items())).encode()
    archive = zip_bytes(entries)
    output.parent.mkdir(parents=True, exist_ok=True)
    # Hard-link promotion is atomic and refuses existing paths on NTFS/POSIX.
    # Both staged files are verified before either name is publicly promoted.
    with tempfile.TemporaryDirectory(prefix='.spdif-package-', dir=output.parent) as staging:
        staged = Path(staging)/output.name
        staged.write_bytes(archive)
        staged_sum = staged.with_suffix(staged.suffix+'.sha256')
        staged_sum.write_bytes((sha(archive)+'  '+output.name+'\n').encode('ascii'))
        verify(staged, expected_source=source['source_sha'], expected_tag=tag, require_release=release)
        created = []
        try:
            for src, dst in ((staged_sum, checksum), (staged, output)):
                os.link(src, dst)
                created.append(dst)
        except BaseException:
            for path in created:
                path.unlink()
            raise
    return output


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    selection = parser.add_mutually_exclusive_group()
    selection.add_argument('--dll', type=Path, help='Legacy Windows plugin path')
    selection.add_argument('--plugin', type=Path, help='Built production plugin on any supported platform')
    parser.add_argument('--output', type=Path)
    parser.add_argument('--release', action='store_true', help='Require a clean matching tagged checkout')
    parser.add_argument('--tag')
    parser.add_argument('--source-sha', help='Require this exact checked-out source commit')
    parser.add_argument('--verify', type=Path, help='Verify an existing archive and external checksum only')
    args = parser.parse_args()
    try:
        if args.verify:
            result = verify(args.verify, expected_source=args.source_sha, expected_tag=args.tag, require_release=args.release)
            print(json.dumps(result, sort_keys=True))
        else:
            plugin = args.plugin or args.dll or ROOT/'build-win/Analyzers/Release/SpdifAnalyzer.dll'
            # Preserve the old default location for the legacy Windows command.
            output = args.output
            if output is None and not args.plugin and not args.release:
                output = ROOT/'artifacts/SpdifAnalyzer-windows-x64.zip'
            print(package(plugin, output, release=args.release, tag=args.tag, expected_source=args.source_sha))
    except (OSError, ValueError, KeyError, zipfile.BadZipFile, subprocess.CalledProcessError) as error:
        parser.exit(2, 'package: '+str(error)+'\n')


if __name__ == '__main__':
    main()
