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

  // The Green Line branches are separate MBTA routes; they're offered as one line
  // whose value is the comma-separated route list the API accepts as a filter.
  var GREEN_PREFIX = 'Green-';
  var GREEN_ID = 'Green-B,Green-C,Green-D,Green-E';

  // Outbound terminals of commuter rail lines that split into several branches.
  // name is shown in the dropdown and matched to trip headsigns in index.js;
  // code is what the watch shows in the label.
  var COMMUTER_ENDPOINTS = {
    'CR-Newburyport': [{ name: 'Newburyport', code: 'NEWB' }, { name: 'Rockport', code: 'ROCK' }],
    'CR-Fitchburg': [{ name: 'Wachusett', code: 'WACH' }, { name: 'Littleton/Route 495', code: 'LITT' }],
    'CR-Haverhill': [{ name: 'Haverhill', code: 'HAVR' }, { name: 'Reading', code: 'READ' }],
    'CR-Franklin': [
      { name: 'Forge Park/495', code: 'FORG' }, { name: 'Foxboro', code: 'FOXB' },
      { name: 'Providence', code: 'PROV' }
    ],
    'CR-Providence': [
      { name: 'Stoughton', code: 'STOU' }, { name: 'Wickford Junction', code: 'WICK' },
      { name: 'Providence', code: 'PROV' }
    ],
    'CR-NewBedford': [{ name: 'New Bedford', code: 'NBED' }, { name: 'Fall River', code: 'FALL' }],
    'CR-Kingston': [
      { name: 'Kingston', code: 'KING' }, { name: 'New Bedford', code: 'NBED' },
      { name: 'Fall River', code: 'FALL' }
    ]
  };

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

  // The short code for an endpoint; unknown names are cut down to four letters.
  function endpointCode(routeId, name) {
    var endpoints = COMMUTER_ENDPOINTS[routeId] || [];
    for (var i = 0; i < endpoints.length; i++) {
      if (endpoints[i].name === name) return endpoints[i].code;
    }
    return name.substring(0, 4).toUpperCase();
  }

  function updateLabel(row) {
    var routeId = rowItem('ROUTE', row).get();
    var route = routes[routeId];
    var stopName = selectedText(rowItem('STOP', row));
    if (!route || !stopName) return;
    ABBREVIATIONS.forEach(function(a) {
      stopName = stopName.replace(a[0], a[1]);
    });
    // A direction value is "0", or "0:<endpoint name>" for a single endpoint.
    var parts = rowItem('DIR', row).get().split(':');
    var isEndpoint = parts.length > 1;
    var dirName = route.direction_names[parts[0]] || '';
    var dir = dirName === 'Outbound' ? 'OUT' : dirName === 'Inbound' ? 'IN' : dirName;
    if (isEndpoint) dir = endpointCode(routeId, parts.slice(1).join(':'));
    // Keep labels short enough to sit beside the countdown in the watch header:
    // shorten the direction first, then the station name. Endpoint codes stay whole.
    if (!isEndpoint && stopName.length + 3 + dir.length > LABEL_FIT) {
      dir = dir.length > 3 ? dir.charAt(0) : dir;
    }
    stopName = stopName.substring(0, Math.max(LABEL_FIT - 3 - dir.length, 4)).trim();
    rowItem('LABEL', row).set((stopName + ' > ' + dir).toUpperCase().substring(0, LABEL_MAX));
  }

  // Outbound gets an extra option per endpoint so a row can show just one branch.
  function directionOptions(routeId, route) {
    var options = [{
      value: '0',
      label: route.direction_names[0] + ' → ' + route.direction_destinations[0]
    }];
    (COMMUTER_ENDPOINTS[routeId] || []).forEach(function(endpoint) {
      options.push({
        value: '0:' + endpoint.name,
        label: route.direction_names[0] + ' → ' + endpoint.name + ' only'
      });
    });
    options.push({
      value: '1',
      label: route.direction_names[1] + ' → ' + route.direction_destinations[1]
    });
    return options;
  }

  function isGreenBranch(routeId) {
    return routeId.indexOf(GREEN_PREFIX) === 0 && routeId.indexOf(',') < 0;
  }

  function loadStops(routeId, callback) {
    if (stopsByRoute[routeId]) {
      callback(stopsByRoute[routeId]);
      return;
    }
    if (routeId.indexOf(',') >= 0) {
      loadMergedStops(routeId, callback);
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

  // Stations of a merged line: stops served by several branches first, in line
  // order, then a group for each branch's own stops.
  function loadMergedStops(routeId, callback) {
    var branchIds = routeId.split(',');
    var lists = [];
    var pending = branchIds.length;
    var failed = false;
    branchIds.forEach(function(branchId, i) {
      loadStops(branchId, function(stops) {
        if (!stops) failed = true;
        lists[i] = stops || [];
        if (--pending > 0) return;
        if (failed) {
          callback(null);
          return;
        }
        var count = {};
        lists.forEach(function(list) {
          list.forEach(function(s) { count[s.value] = (count[s.value] || 0) + 1; });
        });
        // Interleave the branches' shared stops, keeping each branch's order.
        var shared = [];
        lists.forEach(function(list) {
          var prev = -1;
          list.forEach(function(s) {
            if (count[s.value] < 2) return;
            var at = -1;
            for (var j = 0; j < shared.length; j++) {
              if (shared[j].value === s.value) at = j;
            }
            if (at < 0) {
              at = prev + 1;
              shared.splice(at, 0, s);
            }
            prev = at;
          });
        });
        var groups = [{ label: 'Shared', options: shared }];
        lists.forEach(function(list, j) {
          var own = list.filter(function(s) { return count[s.value] < 2; });
          var letter = branchIds[j].substring(GREEN_PREFIX.length);
          if (own.length) groups.push({ label: letter + ' branch', options: own });
        });
        stopsByRoute[routeId] = groups;
        callback(groups);
      });
    });
  }

  // Combines several routes' details into one line, e.g. the Green Line branches.
  // Listing every branch's terminus wraps badly in the select, so destinations
  // are summarised.
  function mergeRoutes(ids, label) {
    var first = routes[ids[0]];
    return {
      long_name: label,
      type: first.type,
      direction_names: first.direction_names,
      direction_destinations: ['all branches', 'all branches']
    };
  }

  // Refreshes a row's direction and station lists for its current line.
  function onRouteChanged(row, byUser) {
    var routeId = rowItem('ROUTE', row).get();
    var route = routes[routeId];
    if (!route) return;
    setOptions(rowItem('DIR', row), directionOptions(routeId, route), rowItem('DIR', row).get());
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

  // Bluebikes docks: each slot has a search box that filters the select below it.
  var NUM_BIKES = 9;
  var BIKE_MATCH_LIMIT = 40;
  var BIKE_INFO_URL = 'https://gbfs.lyft.com/gbfs/1.1/bos/en/station_information.json';
  var bikeStations = null;

  function setBikeStatus(text) {
    var status = clayConfig.getItemById('bike-status');
    if (!status) return;
    if (text) {
      status.set(text);
      status.show();
    } else {
      status.hide();
    }
  }

  function bikeLabel(value) {
    return value.split('|').slice(1).join('|') || value;
  }

  // Offers "None", the slot's current dock, and the docks whose names contain
  // every word typed.
  function filterBikes(slot) {
    var item = clayConfig.getItemByMessageKey('BIKE[' + slot + ']');
    var current = item.get();
    var words = clayConfig.getItemById('bike-search-' + slot).get().toLowerCase()
      .split(/\s+/).filter(function(w) { return w; });
    var options = [{ value: '', label: 'None' }];
    if (current) options.push({ value: current, label: bikeLabel(current) });
    var matches = 0;
    if (bikeStations && words.length) {
      bikeStations.forEach(function(station) {
        if (station.value === current) return;
        var name = station.label.toLowerCase();
        for (var i = 0; i < words.length; i++) {
          if (name.indexOf(words[i]) < 0) return;
        }
        if (++matches <= BIKE_MATCH_LIMIT) options.push(station);
      });
      var shown = Math.min(matches, BIKE_MATCH_LIMIT);
      setBikeStatus(matches === 0 ? 'No docks match "' + words.join(' ') + '".'
                  : shown + (matches > shown ? ' of ' + matches : '') + ' matching dock' +
                    (matches === 1 ? ' is' : 's are') + ' in the Cluster ' + (Math.floor(slot / 3) + 1) + ', dock ' + (slot % 3 + 1) +
                    ' list. Tap it to pick one.');
    }
    setOptions(item, options, current);
  }

  function loadBikeStations() {
    var xhr = new XMLHttpRequest();
    xhr.open('GET', BIKE_INFO_URL, true);
    xhr.onload = function() {
      try {
        bikeStations = JSON.parse(xhr.responseText).data.stations.map(function(s) {
          return { value: s.station_id + '|' + s.name, label: s.name };
        }).sort(function(a, b) {
          return a.label < b.label ? -1 : a.label > b.label ? 1 : 0;
        });
        setBikeStatus('');
      } catch (e) {
        setBikeStatus('Could not load Bluebikes docks. Check your connection.');
      }
    };
    xhr.onerror = function() {
      setBikeStatus('Could not load Bluebikes docks. Check your connection.');
    };
    xhr.send();
  }

  clayConfig.on(clayConfig.EVENTS.AFTER_BUILD, function() {
    for (var slot = 0; slot < NUM_BIKES; slot++) {
      (function(s) {
        var search = clayConfig.getItemById('bike-search-' + s);
        if (!search) return;
        // 'input' fires on every keystroke; Clay's own change event only on blur.
        search.$manipulatorTarget[0].addEventListener('input', function() {
          filterBikes(s);
        });
      })(slot);
    }
    loadBikeStations();

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
      var greenIds = [];
      data.forEach(function(r) {
        routes[r.id] = r.attributes;
        if (isGreenBranch(r.id)) {
          greenIds.push(r.id);
          if (greenIds.length > 1) return;
          r = { id: GREEN_ID, attributes: { long_name: 'Green Line', type: r.attributes.type } };
        }
        (r.attributes.type === 2 ? commuter : subway)
          .push({ value: r.id, label: r.attributes.long_name });
      });
      if (greenIds.length) {
        // Keep the value in step with the branches the API actually lists.
        var mergedId = greenIds.join(',');
        subway.forEach(function(o) {
          if (o.value === GREEN_ID) o.value = mergedId;
        });
        GREEN_ID = mergedId;
        routes[GREEN_ID] = mergeRoutes(greenIds, 'Green Line');
      }
      var groups = [
        { label: 'Subway', options: subway },
        { label: 'Commuter Rail', options: commuter }
      ];
      for (var row = 0; row < NUM_ROWS; row++) {
        var saved = rowItem('ROUTE', row).get();
        // Rows saved on a single branch move to the merged line, keeping their station.
        if (isGreenBranch(saved)) saved = GREEN_ID;
        setOptions(rowItem('ROUTE', row), groups, saved);
        onRouteChanged(row, false);
      }
      setStatus('');
    });
  });
};
