#!/usr/bin/env python3
"""Preserve ordered Git process configuration across native environment resets.

CTest 3.31.6 on Windows drops inherited empty environment entries when restoring
the environment after a test with ENVIRONMENT properties. Move the indexed Git
configuration, including empty credential-helper resets, into Git's nonempty
GIT_CONFIG_PARAMETERS representation before launching CTest. No values are
printed, persisted, placed on a command line, removed from Git's effective
configuration, or guessed if already missing. The parent environment is unchanged.
"""
from __future__ import annotations

import os
import re
import subprocess
import sys


def preserve(environment: dict[str, str]) -> dict[str, str]:
    result = dict(environment)
    count_text = environment.get('GIT_CONFIG_COUNT', '0')
    if not re.fullmatch(r'[0-9]+', count_text) or int(count_text) > 10000:
        raise ValueError('Invalid Git process configuration count')
    count = int(count_text)
    if not count:
        return result
    # git/config.c: git_config_from_parameters() reads indexed entries BEFORE
    # GIT_CONFIG_PARAMETERS. The separate quoted key/value syntax also preserves
    # '=' inside a subsection, unlike a legacy combined key=value token.
    parameters = environment.get('GIT_CONFIG_PARAMETERS', '')
    tokens = []
    for index in range(count):
        key_name, value_name = f'GIT_CONFIG_KEY_{index}', f'GIT_CONFIG_VALUE_{index}'
        if key_name not in environment or value_name not in environment:
            raise ValueError('Incomplete inherited Git process configuration; cannot reconstruct missing values')
        key, value = environment[key_name], environment[value_name]
        if not key or '\x00' in key or '\x00' in value:
            raise ValueError('Invalid Git process configuration entry')
        quote = lambda text: "'" + text.replace("'", "'\\''") + "'"
        tokens.append(quote(key) + '=' + quote(value))
    result['GIT_CONFIG_PARAMETERS'] = ' '.join([*tokens, parameters]).rstrip()
    # Zero entries in the indexed transport; all of them survive in PARAMETERS.
    # Leave original entries intact as well, without exposing their values.
    result['GIT_CONFIG_COUNT'] = '0'
    return result


def main() -> int:
    command = sys.argv[1:]
    if command and command[0] == '--':
        command = command[1:]
    if not command:
        print('Git environment wrapper requires a command.', file=sys.stderr)
        return 2
    try:
        environment = preserve(dict(os.environ))
        return subprocess.run(command, env=environment, check=False).returncode
    except (OSError, ValueError) as error:
        # Error strings deliberately contain no configuration values.
        print('Git environment transport failed: ' + str(error), file=sys.stderr)
        return 2


if __name__ == '__main__':
    raise SystemExit(main())
