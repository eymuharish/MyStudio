"""Archive the committed edition, pinned SDK sources and matching media build inputs."""
import argparse, json, subprocess, tempfile, zipfile
from pathlib import Path

def package(edition, output):
    edition = edition.resolve(); source = edition / 'source'; output = output.resolve()
    if output.exists(): raise FileExistsError('Existing source archives are preserved')
    def git(directory, *args):
        return subprocess.check_output(['git', '-C', str(directory), *args], text=True).strip()
    if git(source, 'status', '--porcelain'): raise RuntimeError('Commit the reviewed source before packaging')
    provenance = json.loads((source / 'docs/opal-dependencies.json').read_text(encoding='utf-8-sig'))
    output.parent.mkdir(parents=True, exist_ok=True)
    prefix = 'MyStudio-source/'
    with tempfile.TemporaryDirectory(prefix='opal-source-', dir=edition / 'build') as temporary:
        with zipfile.ZipFile(output, 'x', compression=zipfile.ZIP_DEFLATED, compresslevel=6) as final:
            archives = [('source', source, git(source, 'rev-parse', 'HEAD'))]
            for name in ('opal', 'ptlib', 'openssl-source'):
                directory = edition / 'dependencies' / name
                head = git(directory, 'rev-parse', 'HEAD')
                if head != provenance['verifiedHeads'][name]: raise RuntimeError('SDK pin mismatch: ' + name)
                archives.append(('dependencies/source-archives/' + name, directory, head))
            for folder, directory, revision in archives:
                archive = Path(temporary) / (directory.name + '.zip')
                subprocess.run(['git', '-C', str(directory), 'archive', '--format=zip',
                                '--output=' + str(archive), revision], check=True)
                with zipfile.ZipFile(archive) as contents:
                    for entry in contents.infolist():
                        if not entry.is_dir(): final.writestr(prefix + folder + '/' + entry.filename, contents.read(entry))
            media = edition / 'dependencies/media'
            for path in sorted(media.rglob('*')):
                if path.is_file(): final.write(path, prefix + 'dependencies/media/' + path.relative_to(media).as_posix())
            final.writestr(prefix + 'README.txt',
                'Reviewed application source is in source/. Matching media headers, libraries and notices are in dependencies/media/.\n'
                'Pinned unmodified SDK sources are in dependencies/source-archives/ for inspection and license compliance.\n'
                'Run source/scripts/Initialize-OpalSdk.ps1 to create the pinned Git checkouts, then follow source/docs/opal-portable-readme.md.\n'
                'Build-OpalDependencies.ps1 applies all recorded SDK adaptations and patches. Platform toolchains and Microsoft CRT are external prerequisites.\n')
            final.writestr(prefix + 'SOURCE-IDENTITY.json', json.dumps({
                'applicationCommit': archives[0][2], 'sdkCommits': provenance['verifiedHeads'],
                'sdkSources': 'Pinned baseline; apply the included build recipe and patches',
                'mediaInputs': 'Exact bundled headers/libraries with recorded hashes and notices'
            }, indent=2))
    print(output)

if __name__ == '__main__':
    parser = argparse.ArgumentParser()
    parser.add_argument('--edition', type=Path, required=True)
    parser.add_argument('--output', type=Path, required=True)
    args = parser.parse_args(); package(args.edition, args.output)
