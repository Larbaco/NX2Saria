import hashlib
import subprocess


def runtime_id(python, pins):
    version = subprocess.run([str(python), '-I', '-c',
                              'import platform; print(platform.python_version())'],
                             check=True, capture_output=True, text=True).stdout.strip()
    digest = hashlib.sha256('\n'.join(sorted(pins)).encode('utf-8')).hexdigest()[:12]
    return f'linux-x86_64-py{version}-{digest}'
