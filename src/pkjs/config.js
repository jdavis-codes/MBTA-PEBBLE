// Defaults for each row; index order must match the row order in src/c/watchface.c.
// The option lists built here are only fallbacks so saved values still show if the
// page is offline. The settings page loads the full MBTA lists live (config-page.js).
var ROWS = [
  {
    route: { value: 'CR-Newburyport', label: 'Newburyport/Rockport Line' },
    stop: { value: 'place-north', label: 'North Station' },
    direction: { value: '0', label: 'Outbound → Newburyport or Rockport' },
    label: 'NORTH STA > OUT',
    warn: { enabled: true, start: 30, end: 25 }
  },
  {
    route: { value: 'CR-Newburyport', label: 'Newburyport/Rockport Line' },
    stop: { value: 'place-ER-0168', label: 'Salem' },
    direction: { value: '1', label: 'Inbound → North Station' },
    label: 'SALEM > IN',
    warn: { enabled: false, start: 15, end: 10 }
  },
  {
    route: { value: 'Red', label: 'Red Line' },
    stop: { value: 'place-cntsq', label: 'Central' },
    direction: { value: '0', label: 'South → Ashmont/Braintree' },
    label: 'CENTRAL > SOUTH',
    warn: { enabled: false, start: 10, end: 5 }
  }
];

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
  var endpoint = /^0:(.+)$/.exec(saved);
  if (endpoint) return 'Outbound → ' + endpoint[1] + ' only';
  return saved;
}

function rowSection(row, i, saved) {
  return {
    type: 'section',
    items: [
      { type: 'heading', defaultValue: 'Row ' + (i + 1) },
      {
        type: 'select',
        messageKey: 'ROUTE[' + i + ']',
        label: 'Line',
        defaultValue: row.route.value,
        options: selectOptions(row.route, saved['ROUTE[' + i + ']'])
      },
      {
        type: 'select',
        messageKey: 'STOP[' + i + ']',
        label: 'Station',
        defaultValue: row.stop.value,
        options: selectOptions(row.stop, saved['STOP[' + i + ']'])
      },
      {
        type: 'select',
        messageKey: 'DIR[' + i + ']',
        label: 'Direction',
        defaultValue: row.direction.value,
        options: selectOptions(row.direction, saved['DIR[' + i + ']'])
      },
      {
        type: 'input',
        messageKey: 'LABEL[' + i + ']',
        label: 'Label on watch',
        description: 'Filled in automatically when you change the station or direction.',
        defaultValue: row.label,
        attributes: { maxlength: 23 }
      },
      {
        type: 'toggle',
        messageKey: 'ALERT_ON[' + i + ']',
        label: 'Show service alerts',
        description: 'Scrolls the MBTA alert for this line and station across the ' +
                     'label once a minute. Minor notices are left out.',
        defaultValue: false
      },
      {
        type: 'toggle',
        messageKey: 'WARN_ON[' + i + ']',
        label: 'Departure warning',
        defaultValue: row.warn.enabled
      },
      {
        type: 'slider',
        messageKey: 'WARN_START[' + i + ']',
        label: 'Start blinking (min before departure)',
        defaultValue: row.warn.start,
        min: 1,
        max: 60,
        step: 1
      },
      {
        type: 'slider',
        messageKey: 'WARN_END[' + i + ']',
        label: 'Stop blinking (min before departure)',
        defaultValue: row.warn.end,
        min: 0,
        max: 59,
        step: 1
      }
    ]
  };
}

// Must match NUM_BIKES in src/pkjs/index.js.
var NUM_BIKES = 6;
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
      defaultValue: 'Up to six docks, shown as small circles along the top left of ' +
                    'the watch in this order. Each fills like a pie with the share ' +
                    'of its spaces holding a bike; a dot beside it means an e-bike ' +
                    'is available. Type part of a name to search, then pick the dock.'
    },
    { type: 'text', id: 'bike-status', defaultValue: 'Loading Bluebikes docks…' }
  ];
  for (var i = 0; i < NUM_BIKES; i++) {
    items.push({
      type: 'input',
      id: 'bike-search-' + i,
      label: 'Dock ' + (i + 1) + ' search',
      attributes: { placeholder: 'e.g. kendall', autocomplete: 'off', autocapitalize: 'off' }
    });
    items.push({
      type: 'select',
      messageKey: 'BIKE[' + i + ']',
      label: 'Dock ' + (i + 1),
      defaultValue: '',
      options: bikeOptions(saved['BIKE[' + i + ']'])
    });
  }
  return { type: 'section', items: items };
}

// saved: the flat settings Clay keeps in localStorage ('clay-settings').
function buildConfig(saved) {
  saved = saved || {};
  return [
    { type: 'heading', defaultValue: 'MBTA Commute' },
    {
      type: 'section',
      items: [
        { type: 'heading', defaultValue: 'Display' },
        {
          type: 'slider',
          messageKey: 'DEPARTURES',
          label: 'Departures shown per row',
          description: 'The text gets smaller to fit 4 or 5 departures on one line.',
          defaultValue: 3,
          min: 1,
          max: 5,
          step: 1
        }
      ]
    },
    {
      type: 'text',
      defaultValue: 'Pick a line, station and direction for each row. Commuter rail ' +
                    'shows departure times; subway shows minutes until departure. ' +
                    'A departure blinks while it is inside its warning range, ' +
                    'blinking faster as the stop time approaches.'
    },
    { type: 'text', id: 'mbta-status', defaultValue: 'Loading MBTA lines…' }
  ].concat(ROWS.map(function(row, i) {
    return rowSection(row, i, saved);
  })).concat([
    bikeSection(saved),
    { type: 'submit', defaultValue: 'Save Settings' }
  ]);
}

module.exports = { ROWS: ROWS, build: buildConfig };
