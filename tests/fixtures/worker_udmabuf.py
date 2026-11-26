import sys
from pathlib import Path

from launcher import worker
from launcher.platform import linux


def run(receipt, args):
    state = {'installed': False, 'cancelled': False}

    def probe():
        installed = state['installed']
        return {'accessible': installed, 'limit_mb': 1024 if installed else 512,
                'needs_fix': not installed, 'offered': not installed, 'reason': '', 'steamos': False}

    def apply(script, action):
        assert script.name == 'udmabuf_fix.sh' and script.is_file(), script
        with receipt.open('a', encoding='utf-8') as log:
            log.write(action + '\n')
        if action == 'remove' and not state['cancelled']:
            state['cancelled'] = True
            return False, 'authorization cancelled'
        state['installed'] = action == 'apply'
        return True, 'synthetic completion'

    linux.udmabuf_status = probe
    linux.udmabuf_fix_installed = lambda: state['installed']
    linux.udmabuf_change_available = lambda: True
    linux.run_udmabuf_fix = apply
    return worker.main(args)


if __name__ == '__main__':
    sys.exit(run(Path(sys.argv[1]), sys.argv[2:]))
