// Runs inside the Clay settings page, not in PebbleKit JS. Clay serialises this
// function into the page, so it must not reference anything outside itself.
// It turns the Line/Station/Direction selects into a cascade loaded live from
// the MBTA API, and fills in the row label when the user changes a row.
module.exports = function() {
  var clayConfig = this;
  var API = 'https://api-v3.mbta.com';
  var NUM_ROWS = 3;
  var LABEL_MAX = 23;
  // Roughly how many characters fit beside the countdown in the watch header.
  var LABEL_FIT = 17;
  var ABBREVIATIONS = [
    [/\bStation\b/g, 'Sta'], [/\bSquare\b/g, 'Sq'], [/\bStreet\b/g, 'St'],
    [/\bCenter\b/g, 'Ctr'], [/\bAvenue\b/g, 'Ave'], [/\bJunction\b/g, 'Jct'],
    [/\bHeights\b/g, 'Hts'], [/\bAirport\b/g, 'Airpt'], [/\bCrossing\b/g, 'Xing'],
    [/\bBoulevard\b/g, 'Blvd'], [/\bUniversity\b/g, 'Univ']
  ];

  var routes = {};
  var stopsByRoute = {};
  // Set while options are replaced in code so listeners can ignore those changes.
  var programmatic = false;

  function apiGet(path, callback) {
    var xhr = new XMLHttpRequest();
    xhr.open('GET', API + path, true);
    xhr.onload = function() {
      var data = null;
      if (xhr.status >= 200 && xhr.status < 300) {
        try {
          data = JSON.parse(xhr.responseText).data;
        } catch (e) {
          data = null;
        }
      }
      callback(data);
    };
    xhr.onerror = function() {
      callback(null);
    };
    xhr.send();
  }

  function rowItem(key, row) {
    return clayConfig.getItemByMessageKey(key + '[' + row + ']');
  }

  function setStatus(text) {
    var status = clayConfig.getItemById('mbta-status');
    if (!status) return;
    if (text) {
      status.set(text);
      status.show();
    } else {
      status.hide();
    }
  }

  // Replaces a select's options, keeping `keep` selected if it is still offered.
  // entries: [{ value, label }] or groups [{ label, options: [{ value, label }] }].
  function setOptions(item, entries, keep) {
    var select = item.$manipulatorTarget[0];
    while (select.firstChild) {
      select.removeChild(select.firstChild);
    }
    var found = false;
    entries.forEach(function(entry) {
      var parent = select;
      if (entry.options) {
        parent = document.createElement('optgroup');
        parent.label = entry.label;
        select.appendChild(parent);
      }
      (entry.options || [entry]).forEach(function(o) {
        var option = document.createElement('option');
        option.value = o.value;
        option.textContent = o.label;
        option.className = 'item-select-option';
        parent.appendChild(option);
        if (o.value === keep) found = true;
      });
    });
    if (found) {
      select.value = keep;
    } else if (select.options.length) {
      select.selectedIndex = 0;
    }
    // Lets the select component refresh the value it displays.
    programmatic = true;
    item.trigger('change');
    programmatic = false;
  }

  function selectedText(item) {
    var select = item.$manipulatorTarget[0];
    var option = select.options[select.selectedIndex];
    return option ? option.textContent : '';
  }

  function updateLabel(row) {
    var route = routes[rowItem('ROUTE', row).get()];
    var stopName = selectedText(rowItem('STOP', row));
    if (!route || !stopName) return;
    ABBREVIATIONS.forEach(function(a) {
      stopName = stopName.replace(a[0], a[1]);
    });
    var dirName = route.direction_names[rowItem('DIR', row).get()] || '';
    var dir = dirName === 'Outbound' ? 'OUT' : dirName === 'Inbound' ? 'IN' : dirName;
    // Keep labels short enough to sit beside the countdown in the watch header:
    // shorten the direction first, then the station name.
    if (stopName.length + 3 + dir.length > LABEL_FIT) {
      dir = dir.length > 3 ? dir.charAt(0) : dir;
    }
    stopName = stopName.substring(0, Math.max(LABEL_FIT - 3 - dir.length, 4)).trim();
    rowItem('LABEL', row).set((stopName + ' > ' + dir).toUpperCase().substring(0, LABEL_MAX));
  }

  function directionOptions(route) {
    return [0, 1].map(function(d) {
      return {
        value: String(d),
        label: route.direction_names[d] + ' → ' + route.direction_destinations[d]
      };
    });
  }

  function loadStops(routeId, callback) {
    if (stopsByRoute[routeId]) {
      callback(stopsByRoute[routeId]);
      return;
    }
    apiGet('/stops?filter[route]=' + encodeURIComponent(routeId) +
           '&filter[direction_id]=0&fields[stop]=name', function(data) {
      if (data) {
        stopsByRoute[routeId] = data.map(function(stop) {
          return { value: stop.id, label: stop.attributes.name };
        });
      }
      callback(stopsByRoute[routeId] || null);
    });
  }

  // Refreshes a row's direction and station lists for its current line.
  function onRouteChanged(row, byUser) {
    var routeId = rowItem('ROUTE', row).get();
    var route = routes[routeId];
    if (!route) return;
    setOptions(rowItem('DIR', row), directionOptions(route), rowItem('DIR', row).get());
    loadStops(routeId, function(stops) {
      // Ignore results for a line the user has already switched away from.
      if (rowItem('ROUTE', row).get() !== routeId) return;
      if (!stops) {
        setStatus('Could not load stations from the MBTA. Check your connection.');
        return;
      }
      setOptions(rowItem('STOP', row), stops, rowItem('STOP', row).get());
      if (byUser) updateLabel(row);
    });
  }

  clayConfig.on(clayConfig.EVENTS.AFTER_BUILD, function() {
    for (var row = 0; row < NUM_ROWS; row++) {
      (function(r) {
        rowItem('ROUTE', r).on('change', function() {
          if (!programmatic) onRouteChanged(r, true);
        });
        rowItem('STOP', r).on('change', function() {
          if (!programmatic) updateLabel(r);
        });
        rowItem('DIR', r).on('change', function() {
          if (!programmatic) updateLabel(r);
        });
      })(row);
    }

    apiGet('/routes?filter[type]=0,1,2&sort=sort_order' +
           '&fields[route]=long_name,type,direction_names,direction_destinations',
    function(data) {
      if (!data) {
        setStatus('Could not load lines from the MBTA. Check your connection.');
        return;
      }
      var subway = [];
      var commuter = [];
      data.forEach(function(r) {
        routes[r.id] = r.attributes;
        (r.attributes.type === 2 ? commuter : subway)
          .push({ value: r.id, label: r.attributes.long_name });
      });
      var groups = [
        { label: 'Subway', options: subway },
        { label: 'Commuter Rail', options: commuter }
      ];
      for (var row = 0; row < NUM_ROWS; row++) {
        setOptions(rowItem('ROUTE', row), groups, rowItem('ROUTE', row).get());
        onRouteChanged(row, false);
      }
      setStatus('');
    });
  });
};
