// Index order must match the SECTION_* constants in src/c/watchface.c.
var STATIONS = [
  { name: 'North Station → Outbound', enabled: true, start: 30, end: 25 },
  { name: 'Salem → Inbound', enabled: false, start: 15, end: 10 },
  { name: 'Central → Southbound', enabled: false, start: 10, end: 5 }
];

function warningSection(station, i) {
  return {
    type: 'section',
    items: [
      { type: 'heading', defaultValue: station.name },
      {
        type: 'toggle',
        messageKey: 'WARN_ON[' + i + ']',
        label: 'Departure warning',
        defaultValue: station.enabled
      },
      {
        type: 'slider',
        messageKey: 'WARN_START[' + i + ']',
        label: 'Start blinking (min before departure)',
        defaultValue: station.start,
        min: 1,
        max: 60,
        step: 1
      },
      {
        type: 'slider',
        messageKey: 'WARN_END[' + i + ']',
        label: 'Stop blinking (min before departure)',
        defaultValue: station.end,
        min: 0,
        max: 59,
        step: 1
      }
    ]
  };
}

module.exports = [
  { type: 'heading', defaultValue: 'MBTA Commute' },
  {
    type: 'section',
    items: [
      { type: 'heading', defaultValue: 'Display' },
      {
        type: 'slider',
        messageKey: 'DEPARTURES',
        label: 'Departures shown per station',
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
    defaultValue: 'A departure blinks while it is inside its warning range, ' +
                  'blinking faster as the stop time approaches.'
  }
].concat(STATIONS.map(warningSection)).concat([
  { type: 'submit', defaultValue: 'Save Settings' }
]);
