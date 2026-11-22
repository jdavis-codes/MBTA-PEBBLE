// Every station starts unset: the user picks their own in the settings page.
// Index order must match the section order in src/c/watchface.c. The option
// lists built here are only fallbacks so saved values still show if the page
// is offline; the settings page loads the full MBTA lists live (config-page.js).
var NONE = { value: '', label: 'None' };
var UNSET_ROW = {
  route: NONE,
  stop: NONE,
  direction: NONE,
  label: '',
  warn: { enabled: false, start: 10, end: 5 }
};
var ROWS = [UNSET_ROW, UNSET_ROW, UNSET_ROW];

// Adds the saved value as an option when it isn't one of the defaults.
function selectOptions(fallback, saved) {
  var options = [fallback];
  if (saved && saved !== fallback.value) {
    options.unshift({ value: saved, label: savedLabel(saved) });
  }
  return options;
}

// Names a saved value that the MBTA lists would otherwise label with its raw id.
function savedLabel(saved) {
  if (/^Green-[A-Z](,Green-[A-Z])+$/.test(saved)) return 'Green Line';
  var endpoint = /^([01]):(.+)$/.exec(saved);
  if (endpoint) {
    return (endpoint[1] === '0' ? 'Outbound' : 'Inbound') + ' → ' + endpoint[2] + ' only';
  }
  return saved;
}

function rowSection(row, i, saved) {
  // The explanations are the same for every station, so only the first shows them.
  function explain(text) {
    return i === 0 ? text : '';
  }
  return {
    type: 'section',
    items: [
      { type: 'heading', id: 'station-' + i, defaultValue: 'Station ' + (i + 1) },
      {
        type: 'select',
        messageKey: 'ROUTE[' + i + ']',
        label: 'Line',
        defaultValue: row.route.value,
        options: selectOptions(row.route, saved['ROUTE[' + i + ']'])
      },
      {
        type: 'select',
        messageKey: 'DIR[' + i + ']',
        label: 'Direction',
        defaultValue: row.direction.value,
        options: selectOptions(row.direction, saved['DIR[' + i + ']'])
      },
      {
        type: 'select',
        messageKey: 'STOP[' + i + ']',
        label: 'Station',
        defaultValue: row.stop.value,
        options: selectOptions(row.stop, saved['STOP[' + i + ']'])
      },
      {
        type: 'input',
        messageKey: 'LABEL[' + i + ']',
        label: 'Timetable header',
        description: explain('The name shown above this station\'s trains. Filled in when ' +
                     'you pick a station or direction; you can edit it.'),
        defaultValue: row.label,
        attributes: { maxlength: 23 }
      },
      {
        type: 'toggle',
        messageKey: 'ALERT_ON[' + i + ']',
        label: 'Show service alerts',
        description: explain('An "!" marks this station when the MBTA posts an alert for ' +
                     'its line. In the line by line layout the alert text also ' +
                     'scrolls across the header every minute. The hour dial does ' +
                     'not show alerts. Minor notices are left out.'),
        defaultValue: false
      },
      {
        type: 'toggle',
        messageKey: 'WARN_ON[' + i + ']',
        label: 'Blinking departure warning',
        description: explain('When enabled, a train time slot starts to blink slowly at ' +
                     'your maximum time to leave, speeds up until your minimum ' +
                     'time to leave, then stops.'),
        defaultValue: row.warn.enabled
      },
      {
        type: 'slider',
        messageKey: 'WARN_START[' + i + ']',
        label: 'Maximum time to leave (minutes before departure)',
        defaultValue: row.warn.start,
        min: 1,
        max: 60,
        step: 1
      },
      {
        type: 'slider',
        messageKey: 'WARN_END[' + i + ']',
        label: 'Minimum time to leave (minutes before departure)',
        defaultValue: row.warn.end,
        min: 0,
        max: 59,
        step: 1
      }
    ]
  };
}

// Must match NUM_BIKES in src/pkjs/index.js.
var NUM_BIKES = 9;
// Docks are grouped into clusters of nearby docks, CLUSTER_SIZE to a cluster.
var CLUSTER_SIZE = 3;
var BIKE_NONE_OPTION = { value: '', label: 'None' };

// A dock's saved value is "<station id>|<station name>", so the name can be
// shown again before (or without) the station list loading.
function bikeOptions(saved) {
  var options = [BIKE_NONE_OPTION];
  if (saved) {
    options.push({ value: saved, label: String(saved).split('|').slice(1).join('|') || saved });
  }
  return options;
}

// The settings page (config-page.js) turns each search box into a live filter
// for the select beneath it.
function bikeSection(saved) {
  var items = [
    { type: 'heading', defaultValue: 'Bluebikes docks' },
    {
      type: 'text',
      defaultValue: 'Up to nine docks, in three clusters of three. Put docks that ' +
                    'are near each other in the same cluster, so the watch shows ' +
                    'them together. Type part of a name to search, then pick the dock.'
    },
    { type: 'text', id: 'bike-status', defaultValue: 'Loading Bluebikes docks…' }
  ];
  for (var i = 0; i < NUM_BIKES; i++) {
    var name = 'Cluster ' + (Math.floor(i / CLUSTER_SIZE) + 1) + ', dock ' + (i % CLUSTER_SIZE + 1);
    if (i % CLUSTER_SIZE === 0) {
      items.push({ type: 'text',
                   defaultValue: '<b>Cluster ' + (i / CLUSTER_SIZE + 1) + '</b>' });
    }
    items.push({
      type: 'input',
      id: 'bike-search-' + i,
      label: name + ' search',
      attributes: { placeholder: 'e.g. kendall', autocomplete: 'off', autocapitalize: 'off' }
    });
    items.push({
      type: 'select',
      messageKey: 'BIKE[' + i + ']',
      label: name,
      defaultValue: '',
      options: bikeOptions(saved['BIKE[' + i + ']'])
    });
  }
  return { type: 'section', items: items };
}

// Must match MARKS in loclog.js.
var MARKERS = ['Entered station', 'On platform', 'Boarded', 'Got off', 'Back at street level'];

// The whole page travels in its address, where most punctuation takes several
// characters. The two that a trace is full of get one each; unpackLines in
// config-page.js undoes it.
function packLines(lines) {
  return encodeURIComponent(lines.join('\n')).replace(/!/g, '%21').replace(/~/g, '%7E')
    .replace(/%2C/g, '!').replace(/%0A/g, '~');
}

// A box of text to select and copy, with a button that tries to copy it.
// The text is put in the box by config-page.js.
function copyBox(id, lines, rows) {
  return [
    {
      type: 'text',
      id: id,
      defaultValue: '<textarea readonly rows="' + rows + '" style="width:100%;box-sizing:border-box;' +
                    'font:12px monospace;color:#000;background:#fff" data-lines="' +
                    packLines(lines) + '"></textarea>'
    },
    { type: 'button', id: id + '-copy', defaultValue: 'Copy' }
  ];
}

// While the log is on, the markers are at the top of the page: they are
// pressed standing on a platform.
function markerSection(log) {
  if (!log.on) return [];
  return [{
    type: 'section',
    items: [
      { type: 'heading', defaultValue: 'Mark where you are' },
      {
        type: 'text',
        id: 'mark-status',
        defaultValue: 'Press a button when it happens, then Save. The time of the ' +
                      'press is what is kept, so you can press several before saving.'
      }
    ].concat(MARKERS.map(function(name, i) {
      return { type: 'button', id: 'mark-' + (i + 1), defaultValue: name };
    })).concat([
      { type: 'input', messageKey: 'LOG_MARKS', id: 'log-marks', label: 'Markers', defaultValue: '' },
      { type: 'button', id: 'mark-save', primary: true, defaultValue: 'Save' }
    ])
  }];
}

function logSection(location, log) {
  var items = [
    { type: 'heading', defaultValue: 'Location log' },
    {
      type: 'toggle',
      messageKey: 'LOG_ON',
      label: 'Record location requests',
      description: 'A development tool. Keeps a record of every time the watchface asks ' +
                   'your phone where it is: the position, how long the answer took and how ' +
                   'accurate it was. It turns itself off after seven days. Turning it on ' +
                   'starts a new log and removes the old one from the phone.',
      defaultValue: false
    },
    { type: 'text', defaultValue: log.status.join('<br>') + '<br>Today: ' + location.join('<br>') },
    {
      type: 'input',
      messageKey: 'LOG_REPO',
      label: 'Upload to GitHub repository',
      description: 'For example yourname/mbta-pebble-data. New records are added to it as ' +
                   'small CSV files in a loclog folder, about every ten minutes and whenever ' +
                   'you save this page. The records hold your positions, so use a private ' +
                   'repository. Leave empty to keep the log on the phone only.',
      defaultValue: '',
      attributes: { placeholder: 'owner/repository', autocomplete: 'off', autocapitalize: 'off' }
    },
    {
      type: 'input',
      messageKey: 'LOG_TOKEN',
      label: 'GitHub token',
      description: 'A fine-grained personal access token that can read and write the ' +
                   'contents of that one repository.',
      defaultValue: '',
      attributes: { type: 'password', placeholder: 'github_pat_...', autocomplete: 'off', autocapitalize: 'off' }
    }
  ];
  if (log.summary.length > 1) {
    items.push({ type: 'heading', size: 6, defaultValue: 'Summary' });
    items.push({ type: 'text', defaultValue: 'Counts and spreads only: no positions and no stop numbers.' });
    items = items.concat(copyBox('log-summary', log.summary, 12));
    items.push({
      type: 'select',
      messageKey: 'LOG_TRACE',
      label: 'Show the trace',
      description: 'The trace is every record in full, as CSV, positions included. ' +
                   'Choose how much, save, and open the settings again: it is shown once, below.',
      defaultValue: '-1',
      options: [
        { label: 'No', value: '-1' },
        { label: 'Last 3 hours', value: '3' },
        { label: 'Last 12 hours', value: '12' },
        { label: 'Last 24 hours', value: '24' },
        { label: 'Everything held', value: '0' }
      ]
    });
    if (log.trace) {
      items.push({ type: 'heading', size: 6, defaultValue: 'Trace' });
      items = items.concat(copyBox('log-trace', log.trace, 8));
    }
  }
  return { type: 'section', items: items };
}

// saved: the flat settings Clay keeps in localStorage ('clay-settings').
// usage: lines describing how much data has been downloaded, from index.js.
// location: lines from today's location requests, also from index.js.
// log: the location log, from loclog.js: { on, status, summary, trace }, each but `on` a list of lines.
function buildConfig(saved, usage, location, log) {
  saved = saved || {};
  usage = usage || [];
  location = location || [];
  log = log || { on: false, status: [], summary: [], trace: null };
  return [
    { type: 'heading', defaultValue: 'MBTA Pebble' }
  ].concat(markerSection(log)).concat([
    {
      type: 'section',
      items: [
        { type: 'heading', defaultValue: 'Display' },
        {
          type: 'select',
          messageKey: 'LAYOUT',
          label: 'Layout',
          defaultValue: '0',
          options: [
            { label: 'Line by line', value: '0' },
            { label: 'Departure board', value: '1' },
            { label: 'Hour dial', value: '2' },
            { label: 'Departure board: nearby', value: '3' }
          ]
        },
        {
          type: 'slider',
          messageKey: 'RADIUS',
          label: 'Nearby radius (meters)',
          description: 'The board lists every train and bus leaving from stops ' +
                       'within this distance of your phone, in place of your saved ' +
                       'stations. The watch shows the radius above the list. Your ' +
                       'phone\'s position is sent to the MBTA to find the stops.',
          defaultValue: 400,
          min: 100,
          max: 1000,
          step: 100
        },
        {
          type: 'slider',
          messageKey: 'BOARD_ROWS',
          label: 'Departures shown on the board',
          description: 'With 7 or 8 departures the lines get shorter and the times ' +
                       'smaller, which also leaves more room for the destination.',
          defaultValue: 6,
          min: 6,
          max: 8,
          step: 1
        },
        {
          type: 'select',
          messageKey: 'BOARD_SHOW',
          label: 'Show each departure as',
          description: 'Showing only one of the two leaves more room for the ' +
                       'destination.',
          defaultValue: '0',
          options: [
            { label: 'Time and minutes', value: '0' },
            { label: 'Minutes only', value: '1' },
            { label: 'Time only', value: '2' }
          ]
        },
        {
          type: 'slider',
          messageKey: 'DEPARTURES',
          label: 'Trains shown per station',
          description: 'With 4 or 5 trains the text gets smaller to fit.',
          defaultValue: 3,
          min: 1,
          max: 5,
          step: 1
        }
      ]
    },
    { type: 'heading', id: 'stations-heading', size: 4,
      defaultValue: 'Choose up to 3 stations or bus stops to watch' },
    { type: 'text', id: 'mbta-status', defaultValue: 'Loading MBTA lines…' }
  ]).concat(ROWS.map(function(row, i) {
    return rowSection(row, i, saved);
  })).concat([
    bikeSection(saved),
    {
      type: 'section',
      items: [
        { type: 'heading', defaultValue: 'Data usage' },
        {
          type: 'text',
          defaultValue: usage.join('<br>')
        }
      ]
    },
    logSection(location, log),
    { type: 'submit', defaultValue: 'Save' }
  ]);
}

module.exports = { ROWS: ROWS, build: buildConfig };
