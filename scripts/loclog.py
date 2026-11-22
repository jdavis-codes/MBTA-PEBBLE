#!/usr/bin/env python3
"""Load the location log into pandas.

The watchface's location log (src/pkjs/loclog.js) is CSV: the files it uploads
to a GitHub repository under loclog/, or text copied from the settings page
and saved to a file. Both have the same columns.

    from loclog import load
    df, marks = load('path/to/data-repo/loclog')

    scripts/loclog.py PATH [PATH ...]     # print a summary

df has one row per location request, indexed by local time; marks has one row
per marker. Columns added to df:
    state      where the last marker said you were ('' when unknown)
    dt         seconds since the previous answered request
    moved      meters from the previous answered position
    speed_calc moved / dt, in m/s
"""
import glob
import os
import sys

import numpy as np
import pandas as pd

# Must match MARKS, STATE_LASTS_MS and STREET_LASTS_MS in src/pkjs/loclog.js.
STATES = {
    'Entered station': 'In a station',
    'On platform': 'On a platform',
    'Boarded': 'On a train',
    'Got off': 'On a platform',
    'Back at street level': 'At street level',
}
STATE_LASTS_S = 2 * 60 * 60
STREET_LASTS_S = 20 * 60


def _files(paths):
    for path in paths:
        if os.path.isdir(path):
            yield from sorted(glob.glob(os.path.join(path, '**', '*.csv'), recursive=True))
        else:
            yield path


def load(*paths):
    """Returns (requests, marks) from CSV files or directories of them."""
    frames = [pd.read_csv(f, dtype={'stop': str, 'mark': str}) for f in _files(paths)]
    if not frames:
        raise SystemExit('No CSV files found.')
    everything = pd.concat(frames, ignore_index=True).drop_duplicates(['t', 'result', 'mark'])
    everything = everything.sort_values('t')
    everything['time'] = pd.to_datetime(everything['time'])
    is_mark = everything['result'] == 'mark'
    marks = everything[is_mark][['t', 'time', 'mark']].reset_index(drop=True)
    df = everything[~is_mark].drop(columns='mark').reset_index(drop=True)

    df['state'] = ''
    for i, mark in marks.iterrows():
        state = STATES.get(mark['mark'], '')
        lasts = STREET_LASTS_S if state == 'At street level' else STATE_LASTS_S
        until = marks['t'].iloc[i + 1] if i + 1 < len(marks) else np.inf
        df.loc[(df['t'] >= mark['t']) & (df['t'] < until), 'state'] = ''
        df.loc[(df['t'] >= mark['t']) & (df['t'] < min(until, mark['t'] + lasts)), 'state'] = state

    ok = df[df['result'] == 'ok']
    dt = ok['t'].diff()
    north = ok['lat'].diff() * 111000
    east = ok['lon'].diff() * 111000 * np.cos(np.radians(ok['lat']))
    df['dt'] = dt
    df['moved'] = np.hypot(north, east)
    df['speed_calc'] = df['moved'] / df['dt']
    return df.set_index('time', drop=False), marks


def summary(df):
    """The figures the settings page shows, for any slice of the log."""
    ok = df[df['result'] == 'ok']
    out = {'requests': len(df), 'answered': len(ok)}
    for result in ('failed', 'timeout', 'no answer', 'denied'):
        out[result] = int((df['result'] == result).sum())
    if len(ok):
        out.update({
            'acc_median_m': ok['acc'].median(), 'acc_p90_m': ok['acc'].quantile(0.9),
            'acc_worst_m': ok['acc'].max(),
            'took_median_s': ok['took_ms'].median() / 1000, 'took_p90_s': ok['took_ms'].quantile(0.9) / 1000,
            'longest_gap_min': ok['dt'].max() / 60,
            'age_median_s': ok['age'].median(),
            'speed_reported': ok['speed'].notna().mean(), 'heading_reported': ok['heading'].notna().mean(),
        })
    return pd.Series(out)


if __name__ == '__main__':
    if len(sys.argv) < 2:
        raise SystemExit(__doc__)
    requests, markers = load(*sys.argv[1:])
    pd.set_option('display.float_format', '{:.1f}'.format, 'display.width', 200)
    print('%d requests, %d markers, %s to %s' % (len(requests), len(markers),
                                                requests['time'].min(), requests['time'].max()))
    table = {'all': summary(requests)}
    for state, part in requests[requests['state'] != ''].groupby('state'):
        table[state] = summary(part)
    print(pd.DataFrame(table).to_string())
