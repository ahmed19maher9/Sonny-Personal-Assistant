#include "BrowserPageScripts.h"

#include "../resources/avatars/json.hpp"

namespace Jarvis {
namespace Browser {
namespace BrowserPageScripts {

namespace {

// The injected runtime. Kept as raw string literals so it stays readable and
// diffable; "__SONNY_SYNONYMS__" is substituted with the live synonym table
// from UserProfileStore. MSVC caps a single string literal at 16 KB (C2026),
// hence the split into parts which runtime_js() concatenates.
const char* kRuntimeJsPart1 = R"JS(
var SYNONYMS = __SONNY_SYNONYMS__;
var AUTOCOMPLETE = {
  'given-name':'first_name','additional-name':'middle_name','family-name':'last_name',
  'name':'full_name','nickname':'first_name','email':'email','tel':'phone',
  'tel-national':'phone','tel-local':'phone','street-address':'address',
  'address-line1':'address','address-line2':'address2','address-line3':'address2',
  'address-level1':'state','address-level2':'city','address-level3':'city',
  'postal-code':'zip','country':'country','country-name':'country',
  'organization':'company','organization-title':'job_title','url':'website',
  'bday':'birth_date','sex':'gender','username':'username',
  'current-password':'password','new-password':'password',
  'photo':'photo','impp':'website','language':'language'
};

function norm(value) {
  if (value === null || value === undefined) return '';
  return String(value).toLowerCase()
    .replace(/[\u2018\u2019\u201B`]/g, "'")
    .replace(/[^a-z0-9'@.+-]+/g, ' ')
    .replace(/\s+/g, ' ')
    .trim();
}
function squash(value) { return norm(value).replace(/[^a-z0-9]/g, ''); }

function isConnected(el) { return !!(el && el.isConnected !== false && el.ownerDocument); }

function rectOf(el) {
  try { return el.getBoundingClientRect(); }
  catch (e) { return {left:0,top:0,width:0,height:0}; }
}

function isVisible(el) {
  if (!isConnected(el)) return false;
  var type = (el.getAttribute && el.getAttribute('type') || '').toLowerCase();
  if (type === 'hidden') return false;
  var style = null;
  try { style = window.getComputedStyle(el); } catch (e) { style = null; }
  if (style) {
    if (style.display === 'none' || style.visibility === 'hidden') return false;
    if (style.visibility === 'collapse') return false;
    if (parseFloat(style.opacity) === 0) return false;
  }
  if (el.getAttribute && el.getAttribute('aria-hidden') === 'true') return false;
  var rect = rectOf(el);
  if (rect.width <= 0 && rect.height <= 0) return false;
  return true;
}

function isEnabled(el) {
  if (el.disabled) return false;
  if (el.getAttribute && el.getAttribute('aria-disabled') === 'true') return false;
  if (el.classList && el.classList.contains('disabled')) return false;
  return true;
}

function isFillable(el) {
  var tag = el.tagName;
  var type = (el.getAttribute('type') || '').toLowerCase();
  if (tag === 'TEXTAREA' || tag === 'SELECT') return true;
  if (el.isContentEditable) return true;
  if (tag !== 'INPUT') return false;
  return ['', 'text', 'search', 'email', 'tel', 'url', 'password', 'number', 'date',
          'datetime-local', 'month', 'week', 'time', 'checkbox', 'radio', 'color',
          'range'].indexOf(type) !== -1;
}

function cssEscape(value) {
  if (window.CSS && CSS.escape) return CSS.escape(value);
  return String(value).replace(/[^a-zA-Z0-9_-]/g, function (c) { return '\\' + c; });
}
// __SONNY_CHUNK_END__
function labelTextOf(el) {
  var parts = [];
  try {
    if (el.id) {
      var forLabel = document.querySelector('label[for="' + cssEscape(el.id) + '"]');
      if (forLabel) parts.push(forLabel.innerText || forLabel.textContent || '');
    }
    var wrapping = el.closest ? el.closest('label') : null;
    if (wrapping) parts.push(wrapping.innerText || wrapping.textContent || '');
    var refs = el.getAttribute ? el.getAttribute('aria-labelledby') : '';
    if (refs) {
      refs.split(/\s+/).forEach(function (id) {
        var node = document.getElementById(id);
        if (node) parts.push(node.innerText || node.textContent || '');
      });
    }
  } catch (e) {}
  return parts.join(' ').trim();
}

function rawAccessibleName(el) {
  var name = (el.getAttribute && el.getAttribute('aria-label')) || '';
  if (!name) name = labelTextOf(el);
  if (!name) name = (el.getAttribute && el.getAttribute('placeholder')) || '';
  if (!name) name = (el.getAttribute && el.getAttribute('title')) || '';
  if (!name) name = (el.getAttribute && el.getAttribute('alt')) || '';
  if (!name && el.tagName === 'INPUT') {
    var type = (el.getAttribute('type') || '').toLowerCase();
    if (type === 'submit' || type === 'button' || type === 'reset') name = el.value || '';
  }
  if (!name) {
    var text = el.innerText || el.textContent || '';
    name = text.replace(/\s+/g, ' ').trim();
    if (name.length > 160) name = name.slice(0, 160);
  }
  return name.replace(/\s+/g, ' ').trim();
}

function accessibleName(el) { return norm(rawAccessibleName(el)); }

function cssPath(el) {
  if (!el || el.nodeType !== 1) return '';
  if (el.id) {
    try {
      if (document.querySelectorAll('#' + cssEscape(el.id)).length === 1) return '#' + el.id;
    } catch (e) {}
  }
  var attrs = ['data-testid', 'data-test-id', 'data-test', 'name', 'aria-label', 'placeholder'];
  for (var a = 0; a < attrs.length; a++) {
    var value = el.getAttribute && el.getAttribute(attrs[a]);
    if (value && value.length < 60) {
      var candidate = el.tagName.toLowerCase() + '[' + attrs[a] + '="' +
        String(value).replace(/"/g, '\\"') + '"]';
      try {
        if (document.querySelectorAll(candidate).length === 1) return candidate;
      } catch (e) {}
    }
  }
  var parts = [];
  var node = el;
  var depth = 0;
  while (node && node.nodeType === 1 && depth < 5) {
    var tag = node.tagName.toLowerCase();
    if (node.id) { parts.unshift('#' + node.id); break; }
    var parent = node.parentElement;
    if (parent) {
      var sameTag = Array.prototype.filter.call(parent.children, function (child) {
        return child.tagName === node.tagName;
      });
      if (sameTag.length > 1) tag += ':nth-of-type(' + (sameTag.indexOf(node) + 1) + ')';
    }
    parts.unshift(tag);
    node = parent;
    depth++;
  }
  return parts.join(' > ');
}

function describe(el, index) {
  var rect = rectOf(el);
  var type = (el.getAttribute('type') || '').toLowerCase();
  var value = '';
  try {
    if (el.tagName === 'SELECT' || el.tagName === 'INPUT' || el.tagName === 'TEXTAREA') {
      value = String(el.value === undefined || el.value === null ? '' : el.value).slice(0, 120);
    }
  } catch (e) {}
  return {
    index: index,
    tag: el.tagName.toLowerCase(),
    type: type,
    id: el.id || '',
    name: (el.getAttribute('name') || ''),
    role: (el.getAttribute('role') || '').toLowerCase(),
    label: rawAccessibleName(el).slice(0, 140),
    text: String(el.innerText || el.textContent || '').replace(/\s+/g, ' ').trim().slice(0, 140),
    placeholder: (el.getAttribute('placeholder') || ''),
    href: (el.getAttribute('href') || '').slice(0, 300),
    value: value,
    checked: !!(el.checked),
    visible: isVisible(el),
    enabled: isEnabled(el),
    rect: { x: Math.round(rect.left), y: Math.round(rect.top),
            w: Math.round(rect.width), h: Math.round(rect.height) },
    cx: Math.round(rect.left + rect.width / 2),
    cy: Math.round(rect.top + rect.height / 2),
    selector: cssPath(el)
  };
}
function qsa(selector, scope) {
  try {
    return Array.prototype.slice.call((scope || document).querySelectorAll(selector));
  } catch (e) { return []; }
}

function xpathAll(expression, scope) {
  try {
    var doc = (scope || document);
    var result = document.evaluate(expression, doc, null, XPathResult.ORDERED_NODE_SNAPSHOT_TYPE, null);
    var nodes = [];
    for (var i = 0; i < result.snapshotLength; i++) nodes.push(result.snapshotItem(i));
    return nodes;
  } catch (e) { return []; }
}

var INTERACTIVE = { A:1, BUTTON:1, INPUT:1, SELECT:1, TEXTAREA:1, SUMMARY:1, LABEL:1, OPTION:1 };
var ROLE_TAGS = {
  button: ['button', 'input'], link: ['a'], textbox: ['input', 'textarea'],
  checkbox: ['input'], radio: ['input'], combobox: ['select', 'input'],
  tab: ['button', 'a', 'div'], menuitem: ['a', 'button', 'div'],
  searchbox: ['input'], listbox: ['select'], switch: ['input', 'div'],
  heading: ['h1', 'h2', 'h3', 'h4', 'h5', 'h6']
};

function interactiveHint(el) {
  if (INTERACTIVE[el.tagName]) return 60;
  var role = (el.getAttribute('role') || '').toLowerCase();
  if (role) return 55;
  if (el.hasAttribute('onclick')) return 45;
  if (el.tabIndex >= 0 && el.tagName !== 'BODY') return 30;
  return 0;
}

function textScore(el, wanted) {
  var wanted_norm = norm(wanted);
  if (!wanted_norm) return 0;
  var name = accessibleName(el);
  var own = norm(el.innerText || el.textContent || '');
  var best = 0;
  [name, own].forEach(function (candidate) {
    if (!candidate) return;
    if (candidate === wanted_norm) best = Math.max(best, 100);
    else if (candidate.indexOf(wanted_norm) === 0) best = Math.max(best, 70);
    else if (wanted_norm.indexOf(candidate) === 0 && candidate.length > 3) best = Math.max(best, 55);
    else if (candidate.indexOf(wanted_norm) !== -1) best = Math.max(best, 45);
    else if (squash(candidate).indexOf(squash(wanted_norm)) !== -1 && squash(wanted_norm).length > 3) {
      best = Math.max(best, 35);
    }
  });
  return best;
}

function byText(value) {
  var wanted = norm(value);
  if (!wanted) return [];
  var out = [];
  // Candidate set covers leaf controls plus the structural elements users name
  // when speaking ("the fee table", "order summary"): the `own.length > 400`
  // guard below is what stops a big container from swallowing the match.
  var candidates = qsa('a, button, [role], input, select, textarea, label, span, div, li, td, ' +
                       'th, p, h1, h2, h3, h4, h5, h6, summary, option, caption, table, ' +
                       'thead, tbody, tr, article, section, header, footer, nav, main, form, ' +
                       'figure, figcaption, dl, dt, dd, fieldset, legend, blockquote, pre, ' +
                       'code, strong, em, small, time, address, output');
  for (var i = 0; i < candidates.length; i++) {
    var el = candidates[i];
    var name = accessibleName(el);
    if (!name) continue;
    var own = norm(el.innerText || el.textContent || '');
    if (own.length > 400) continue;  // container, not a leaf target
    if (name === wanted || name.indexOf(wanted) !== -1 ||
        squash(name).indexOf(squash(wanted)) !== -1 ||
        (wanted.indexOf(name) === 0 && name.length > 3)) {
      out.push(el);
    }
  }
  return out;
}

function byLabel(value) {
  var wanted = norm(value);
  if (!wanted) return [];
  var out = [];
  var nodes = qsa('input, select, textarea, button, a, [contenteditable="true"], [role]');
  for (var i = 0; i < nodes.length; i++) {
    var text = norm(labelTextOf(nodes[i]) || (nodes[i].getAttribute('aria-label') || ''));
    if (!text) continue;
    if (text === wanted || text.indexOf(wanted) !== -1 ||
        squash(text).indexOf(squash(wanted)) !== -1) {
      out.push(nodes[i]);
    }
  }
  return out;
}

function byAttr(attribute, value) {
  var wanted = norm(value);
  if (!wanted) return [];
  var out = [];
  var nodes = qsa('input, select, textarea, button, a, [role], [contenteditable="true"], *');
  for (var i = 0; i < nodes.length; i++) {
    var raw = nodes[i].getAttribute && nodes[i].getAttribute(attribute);
    if (!raw) continue;
    var candidate = norm(raw);
    if (candidate === wanted || candidate.indexOf(wanted) !== -1) out.push(nodes[i]);
  }
  return out;
}

function byRole(role, name) {
  var wantedRole = norm(role);
  var tags = ROLE_TAGS[wantedRole] || ['*'];
  var out = [];
  tags.forEach(function (tag) {
    qsa(tag).forEach(function (el) {
      var elRole = norm(el.getAttribute('role') || '') ||
        ({ A: 'link', BUTTON: 'button', SELECT: 'combobox', TEXTAREA: 'textbox' })[el.tagName] ||
        (el.tagName === 'INPUT' ? ({ text: 'textbox', checkbox: 'checkbox', radio: 'radio',
          submit: 'button', search: 'searchbox' }[(el.getAttribute('type') || 'text').toLowerCase()]
          || 'textbox') : '');
      if (elRole !== wantedRole && norm(el.getAttribute('role') || '') !== wantedRole) return;
      if (name) {
        var elName = accessibleName(el);
        var wantedName = norm(name);
        if (elName !== wantedName && elName.indexOf(wantedName) === -1) return;
      }
      out.push(el);
    });
  });
  return out;
}
function byRoleGuess(value) {
  var m = /^([a-z]+)\s+(.+)$/.exec(norm(value));
  return m ? byRole(m[1], m[2]) : [];
}

function resolveCandidates(raw) {
  var results = [];
  var seen = [];
  function add(list) {
    for (var i = 0; i < list.length; i++) {
      var el = list[i];
      if (!el || el.nodeType !== 1) continue;
      if (seen.indexOf(el) === -1) { seen.push(el); results.push(el); }
    }
  }
  var m;
  if ((m = /^(?:css|selector)\s*=\s*([\s\S]+)$/i.exec(raw))) add(qsa(m[1]));
  else if ((m = /^xpath\s*=\s*([\s\S]+)$/i.exec(raw)) ||
           raw.indexOf('//') === 0 || raw.indexOf('(//') === 0) {
    add(xpathAll(m ? m[1] : raw));
  }
  else if ((m = /^text\s*=\s*([\s\S]+)$/i.exec(raw))) add(byText(m[1]));
  else if ((m = /^role\s*=\s*([a-z0-9_-]+)\s*(?:\[\s*name\s*=\s*["']?([^"'\]]+?)["']?\s*\])?$/i.exec(raw))) {
    add(byRole(m[1], m[2]));
  }
  else if ((m = /^(?:label|aria)\s*=\s*([\s\S]+)$/i.exec(raw))) add(byLabel(m[1]));
  else if ((m = /^placeholder\s*=\s*([\s\S]+)$/i.exec(raw))) add(byAttr('placeholder', m[1]));
  else if ((m = /^name\s*=\s*([\s\S]+)$/i.exec(raw))) add(byAttr('name', m[1]));
  else if ((m = /^id\s*=\s*([\s\S]+)$/i.exec(raw))) add(byAttr('id', m[1]));
  else if ((m = /^(?:testid|data-testid)\s*=\s*([\s\S]+)$/i.exec(raw))) add(byAttr('data-testid', m[1]));
  else if ((m = /^title\s*=\s*([\s\S]+)$/i.exec(raw))) add(byAttr('title', m[1]));
  else if ((m = /^alt\s*=\s*([\s\S]+)$/i.exec(raw))) add(byAttr('alt', m[1]));
  else if ((m = /^href\s*=\s*([\s\S]+)$/i.exec(raw))) add(byAttr('href', m[1]));
  else {
    add(qsa(raw));               // plain CSS selector
    add(byText(raw));            // visible text / accessible name
    add(byLabel(raw));           // <label> association
    add(byAttr('placeholder', raw));
    add(byAttr('name', raw));
    add(byAttr('id', raw));
    add(byAttr('data-testid', raw));
    add(byAttr('aria-label', raw));
    add(byRoleGuess(raw));
  }
  return results;
}

function resolve(target, opts) {
  opts = opts || {};
  var raw = (target === null || target === undefined) ? '' : String(target).trim();
  var all = resolveCandidates(raw);
  // Playwright semantics: getByLabel() resolves to the CONTROL, never to the
  // <label>. A label that wraps its control is only a text carrier - the
  // control is what owns value/checked state, receives events and is verified
  // afterwards, so substitute it (first labelable descendant).
  var expanded = [];
  var expandedSeen = [];
  for (var i = 0; i < all.length; i++) {
    var el = all[i];
    if (el.tagName === 'LABEL') {
      var wrapped = el.querySelector('input:not([type="hidden"]), select, textarea, button');
      if (wrapped) el = wrapped;
    }
    if (expandedSeen.indexOf(el) === -1) { expandedSeen.push(el); expanded.push(el); }
  }
  var keep = [];
  for (var i = 0; i < expanded.length; i++) {
    var el = expanded[i];
    if (!opts.includeHidden && !isVisible(el)) continue;
    if (opts.filter === 'fillable' && !isFillable(el)) continue;
    if (opts.filter === 'clickable' && interactiveHint(el) === 0) continue;
    keep.push(el);
  }
  var scored = keep.map(function (el) {
    var score = textScore(el, raw) + interactiveHint(el);
    if (isEnabled(el)) score += 8;
    if (isFillable(el)) score += 12;
    if (opts.filter === 'fillable' && isFillable(el)) score += 40;
    var rect = rectOf(el);
    score += Math.min(15, (rect.width * rect.height) / 5000);
    return { el: el, score: score };
  });
  scored.sort(function (a, b) { return b.score - a.score; });
  return scored;
}

function findAll(target, opts) {
  opts = opts || {};
  var scored = resolve(target, opts);
  // 1-based positions: "index 2" always means the second match, everywhere.
  var out = scored.map(function (item, i) { return describe(item.el, i + 1); });
  if (opts.limit) out = out.slice(0, opts.limit);
  return out;
}

// ------------------------------------------------------------- interaction --

)JS";

// Second half of part 1 (the 16 KB literal cap forces the split).
const char* kRuntimeJsPart1b = R"JS(
function scrollIntoView(el) {
  try {
    el.scrollIntoView({ block: 'center', inline: 'center', behavior: 'instant' });
  } catch (e) {
    try { el.scrollIntoView(); } catch (e2) {}
  }
  return true;
}

function dispatch(el, type, data) {
  var event;
  try {
    if (type === 'input' || type === 'beforeinput') {
      event = new InputEvent(type, { bubbles: true, cancelable: true, data: data || null });
    } else {
      event = new Event(type, { bubbles: true, cancelable: true });
    }
  } catch (e) {
    event = document.createEvent('Event');
    event.initEvent(type, true, true);
  }
  el.dispatchEvent(event);
  return true;
}

function focusEl(el) {
  try {
    el.focus({ preventScroll: false });
  } catch (e) {
    try { el.focus(); } catch (e2) {}
  }
  return document.activeElement === el;
}
)JS";

const char* kRuntimeJsPart2 = R"JS(
function setChecked(el, value) {
  var want = value === true || value === 'true' || value === 'yes' || value === 'on' ||
             value === '1' || value === 1 || value === 'checked';
  if (el.type === 'radio') {
    var target = want ? el : null;
    if (!target) {
      var group = document.getElementsByName(el.name);
      for (var i = 0; i < group.length; i++) {
        if (norm(group[i].value) === norm(value)) { target = group[i]; break; }
      }
      if (!target) target = el;
    }
    if (!target.checked) target.click();
    return true;
  }
  if (el.checked !== want) el.click();
  return true;
}

function toIsoDate(text) {
  var trimmed = text.trim();
  if (/^\d{4}-\d{2}-\d{2}$/.test(trimmed)) return trimmed;
  var slash = /^(\d{1,2})[\/.\-](\d{1,2})[\/.\-](\d{2,4})$/.exec(trimmed);
  if (slash) {
    var year = slash[3].length === 2 ? '20' + slash[3] : slash[3];
    return year + '-' + ('0' + slash[1]).slice(-2) + '-' + ('0' + slash[2]).slice(-2);
  }
  var months = ['january','february','march','april','may','june','july','august',
                'september','october','november','december'];
  var words = /([a-z]+)\s+(\d{1,2}),?\s+(\d{4})/i.exec(trimmed);
  if (words) {
    var index = months.indexOf(words[1].toLowerCase());
    if (index >= 0) {
      return words[3] + '-' + ('0' + (index + 1)).slice(-2) + '-' + ('0' + words[2]).slice(-2);
    }
  }
  return '';
}

function selectOption(el, value) {
  var wanted = norm(value);
  var chosen = -1;
  var fallback = -1;
  for (var i = 0; i < el.options.length; i++) {
    var option = el.options[i];
    var optText = norm(option.text || option.textContent || '');
    var optValue = norm(option.value || '');
    if (optValue === wanted || optText === wanted) { chosen = i; break; }
    if (fallback === -1 && (optText.indexOf(wanted) !== -1 || optValue.indexOf(wanted) !== -1)) {
      fallback = i;
    }
  }
  if (chosen === -1) chosen = fallback;
  if (chosen === -1) return false;
  el.selectedIndex = chosen;
  dispatch(el, 'input');
  dispatch(el, 'change');
  return true;
}

function setValue(el, value) {
  var tag = el.tagName;
  var type = (el.getAttribute('type') || '').toLowerCase();
  var text = (value === null || value === undefined) ? '' : String(value);

  if (tag === 'SELECT') return selectOption(el, text);
  if (type === 'checkbox' || type === 'radio') return setChecked(el, text);

  focusEl(el);
  if (type === 'number' || type === 'range') {
    var numeric = text.replace(/[^0-9.\-]/g, '');
    if (numeric) text = numeric;
  } else if (type === 'date') {
    var iso = toIsoDate(text);
    if (iso) text = iso;
  } else if (type === 'time') {
    var tm = /(\d{1,2}):?(\d{2})?\s*(am|pm)?/i.exec(text);
    if (tm) {
      var hour = parseInt(tm[1], 10);
      var minute = tm[2] ? tm[2] : '00';
      if (tm[3] && tm[3].toLowerCase() === 'pm' && hour < 12) hour += 12;
      if (tm[3] && tm[3].toLowerCase() === 'am' && hour === 12) hour = 0;
      text = ('0' + hour).slice(-2) + ':' + minute;
    }
  }

  try {
    if (el.isContentEditable) {
      el.textContent = text;
      dispatch(el, 'input', text);
      dispatch(el, 'change', text);
      return true;
    }
    var proto = (tag === 'TEXTAREA') ? HTMLTextAreaElement.prototype : HTMLInputElement.prototype;
    var descriptor = null;
    try { descriptor = Object.getOwnPropertyDescriptor(proto, 'value'); } catch (e) {}
    if (descriptor && descriptor.set) descriptor.set.call(el, text);
    else el.value = text;
  } catch (e) {
    try { el.value = text; } catch (e2) { return false; }
  }
  dispatch(el, 'beforeinput', text);
  dispatch(el, 'input', text);
  dispatch(el, 'change', text);
  return true;
}

function currentValue(el) {
  try {
    if (el.type === 'checkbox' || el.type === 'radio') return el.checked ? 'true' : 'false';
    return String(el.value === undefined || el.value === null ? '' : el.value);
  } catch (e) { return ''; }
}

function isAutofilled(el) {
  try {
    if (el.matches && (el.matches(':-webkit-autofill') || el.matches(':autofill'))) return true;
  } catch (e) {}
  return false;
}

function hasUserValue(el) {
  var type = (el.getAttribute('type') || '').toLowerCase();
  if (type === 'checkbox' || type === 'radio') return !!el.checked;
  if (el.tagName === 'SELECT') return el.selectedIndex > 0 ||
    (el.value !== '' && el.options.length > 0 && el.selectedIndex >= 0 &&
     norm(el.options[el.selectedIndex].text || '') !== norm(el.options[0].text || ''));
  var value = currentValue(el).trim();
  if (!value) return false;
  var placeholder = (el.getAttribute('placeholder') || '').trim();
  if (placeholder && norm(value) === norm(placeholder)) return false;
  return true;
}
// ---------------------------------------------------------- form modelling --

)JS";

const char* kRuntimeJsPart3 = R"JS(
function classify(meta) {
  var keys = [];
  var add = function (key, score) {
    if (!key) return;
    for (var i = 0; i < keys.length; i++) {
      if (keys[i].key === key) { keys[i].score = Math.max(keys[i].score, score); return; }
    }
    keys.push({ key: key, score: score });
  };

  var autocomplete = (meta.autocomplete || '').split(' ')[0];
  if (autocomplete && AUTOCOMPLETE[autocomplete]) add(AUTOCOMPLETE[autocomplete], 120);

  var haystacks = [meta.label, meta.name, meta.id, meta.placeholder, meta.aria]
    .filter(function (part) { return part && String(part).trim(); })
    .map(function (part) { return norm(part); });
  var haystack = haystacks.join(' | ');
  var compactHaystack = haystacks.map(squash).join('|');

  Object.keys(SYNONYMS).forEach(function (key) {
    var synonyms = SYNONYMS[key] || [];
    for (var i = 0; i < synonyms.length; i++) {
      var synonym = norm(synonyms[i]);
      if (!synonym) continue;
      var words = synonym.split(' ').length;
      if (haystack === synonym) { add(key, 80 + words * 12); continue; }
      if ((' ' + haystack + ' ').indexOf(' ' + synonym + ' ') !== -1) {
        add(key, 60 + words * 12);
        continue;
      }
      if (compactHaystack.indexOf(squash(synonym)) !== -1 && squash(synonym).length > 3) {
        add(key, 30 + words * 12);
      }
    }
  });

  // Structural hints that frequently disambiguate names on real forms.
  if (meta.type === 'email') add('email', 70);
  if (meta.type === 'tel') add('phone', 70);
  if (meta.type === 'password') add('password', 70);
  if (meta.type === 'checkbox') add('consent', 5);
  keys.sort(function (a, b) { return b.score - a.score; });
  return keys;
}

function formFields(scopeSelector, includeHidden) {
  var scope = document;
  if (scopeSelector) {
    try {
      var found = document.querySelector(scopeSelector);
      if (found) scope = found;
    } catch (e) {}
  }
  var nodes = qsa('input, textarea, select, [contenteditable="true"]', scope);
  var out = [];
  for (var i = 0; i < nodes.length; i++) {
    var el = nodes[i];
    var type = (el.getAttribute('type') || el.tagName.toLowerCase()).toLowerCase();
    if (!includeHidden) {
      if (['hidden', 'submit', 'button', 'reset', 'image', 'file'].indexOf(type) !== -1) continue;
      if (!isVisible(el) && !el.isContentEditable) continue;
    }
    var meta = {
      index: out.length,
      tag: el.tagName.toLowerCase(),
      type: type,
      id: el.id || '',
      name: (el.getAttribute('name') || ''),
      aria: (el.getAttribute('aria-label') || ''),
      autocomplete: (el.getAttribute('autocomplete') || '').toLowerCase(),
      placeholder: (el.getAttribute('placeholder') || ''),
      label: rawAccessibleName(el).slice(0, 140),
      required: !!el.required || el.getAttribute('aria-required') === 'true',
      pattern: (el.getAttribute('pattern') || '').slice(0, 120),
      maxlength: (el.getAttribute('maxlength') || ''),
      value: currentValue(el).slice(0, 160),
      existing: hasUserValue(el),
      autofilled: isAutofilled(el),
      disabled: !!el.disabled,
      readonly: !!el.readOnly,
      options: [],
      selector: cssPath(el)
    };
    if (el.tagName === 'SELECT') {
      meta.options = Array.prototype.slice.call(el.options, 0, 60).map(function (option) {
        return { value: option.value, text: (option.text || '').trim(), selected: option.selected };
      });
    }
    meta.keys = classify(meta);
    out.push(meta);
  }
  return out;
}

function submitControls(scopeSelector) {
  var scope = document;
  if (scopeSelector) {
    try {
      var found = document.querySelector(scopeSelector);
      if (found) scope = found;
    } catch (e) {}
  }
  var candidates = qsa('button[type="submit"], input[type="submit"], input[type="image"], ' +
                       'button:not([type]), form button, [role="button"]', scope);
  var seen = [];
  return candidates.filter(function (el) {
    if (seen.indexOf(el) !== -1) return false;
    seen.push(el);
    if (!isVisible(el) || !isEnabled(el)) return false;
    var name = accessibleName(el);
    return name.length > 0;
  }).slice(0, 12).map(function (el, i) { return describe(el, i); });
}

function autofill(values) {
  var plan = [];
  var skipped = [];
  var nodes = qsa('input, textarea, select, [contenteditable="true"]', document);
  for (var i = 0; i < nodes.length; i++) {
    var el = nodes[i];
    var type = (el.getAttribute('type') || el.tagName.toLowerCase()).toLowerCase();
    if (['hidden', 'submit', 'button', 'reset', 'image', 'file'].indexOf(type) !== -1) continue;
    if (!isVisible(el) && !el.isContentEditable) continue;
    if (el.disabled || el.readOnly) continue;
    if (hasUserValue(el) && !isAutofilled(el)) {
      skipped.push({ label: clip(accessibleName(el), 60), reason: 'already has a value' });
      continue;
    }
    var meta = {
      tag: el.tagName.toLowerCase(),
      type: type,
      id: el.id || '',
      name: (el.getAttribute('name') || ''),
      aria: (el.getAttribute('aria-label') || ''),
      autocomplete: (el.getAttribute('autocomplete') || '').toLowerCase(),
      placeholder: (el.getAttribute('placeholder') || ''),
      label: rawAccessibleName(el)
    };
    var keys = classify(meta);
    for (var k = 0; k < keys.length; k++) {
      var hit = values[keys[k].key];
      if (hit === undefined || hit === null || hit === '') continue;
      try {
        scrollIntoView(el);
        setValue(el, hit);
        el.dispatchEvent(new Event('input', { bubbles: true }));
        el.dispatchEvent(new Event('change', { bubbles: true }));
        plan.push({ filled: clip(accessibleName(el), 60), key: keys[k].key, score: keys[k].score });
      } catch (e) {
        plan.push({ filled: clip(accessibleName(el), 60), key: keys[k].key, error: String(e) });
      }
      break;
    }
  }
  return { filled: plan.length, fields: plan, skipped: skipped };
}

function formOf(el) {
  try { return el.form || (el.closest ? el.closest('form') : null); } catch (e) { return null; }
}
// --------------------------------------------------------------- page API --

)JS";

const char* kRuntimeJsPart4 = R"JS(
function clip(text, max) {
  var value = String(text === null || text === undefined ? '' : text).replace(/\s+/g, ' ').trim();
  if (value.length > max) value = value.slice(0, max) + '...';
  return value;
}

function pageInfo() {
  var doc = document.documentElement || { scrollWidth: 0, scrollHeight: 0 };
  return {
    url: location.href,
    title: document.title,
    readyState: document.readyState,
    lang: document.documentElement ? (document.documentElement.lang || '') : '',
    viewport: { w: window.innerWidth, h: window.innerHeight,
                scrollW: doc.scrollWidth, scrollH: doc.scrollHeight,
                scrollY: Math.round(window.scrollY) },
    forms: document.forms ? document.forms.length : 0,
    links: document.links ? document.links.length : 0,
    images: document.images ? document.images.length : 0,
    iframes: document.querySelectorAll('iframe').length,
    inputs: qsa('input, textarea, select').length
  };
}

function summary(mainTextLimit) {
  var info = pageInfo();
  var headings = qsa('h1, h2, h3').slice(0, 12)
    .map(function (el) { return clip(el.innerText || el.textContent, 120); })
    .filter(function (text) { return text.length > 0; });

  var main = document.querySelector('main, article, [role="main"], #content, #main') || document.body;
  var textLimit = mainTextLimit && mainTextLimit > 200 ? mainTextLimit : 1600;
  var mainText = clip(main ? (main.innerText || main.textContent || '') : '', textLimit);

  var controls = qsa('button, input[type="submit"], input[type="button"], a[role="button"]');
  var buttons = [];
  for (var i = 0; i < controls.length && buttons.length < 14; i++) {
    if (!isVisible(controls[i]) || !isEnabled(controls[i])) continue;
    var name = clip(rawAccessibleName(controls[i]), 70);
    if (name) buttons.push(name);
  }

  var fields = formFields(null, false).slice(0, 30).map(function (field) {
    return { label: clip(field.label || field.name || field.placeholder || field.id, 60),
             type: field.type, required: field.required, filled: field.existing,
             keys: (field.keys || []).slice(0, 2).map(function (k) { return k.key; }) };
  });

  var links = qsa('a[href]').slice(0, 200).map(function (el) {
    return { text: clip(rawAccessibleName(el), 80), href: el.href };
  }).filter(function (link) { return link.text.length > 0 && link.href.indexOf('javascript:') !== 0; });

  return { info: info, headings: headings, text: mainText, buttons: buttons,
           fields: fields, links: links.slice(0, 60),
           submitControls: submitControls().map(function (control) { return control.label; }) };
}
function extract(kind, selector, limit) {
  var max = limit && limit > 0 ? limit : 40;
  var scope = document;
  var scopeLabel = selector || '';
  if (selector) {
    try {
      var found = document.querySelector(selector);
      if (!found) return { ok: false, error: 'selector not found: ' + selector };
      scope = found;
    } catch (e) {
      return { ok: false, error: 'invalid selector: ' + selector };
    }
  }

  switch (kind) {
    case 'links':
      return { ok: true, data: qsa('a[href]', scope).slice(0, max).map(function (el) {
        return { text: clip(rawAccessibleName(el), 120), href: el.href };
      }) };
    case 'tables':
      return { ok: true, data: qsa('table', scope).slice(0, Math.min(max, 8)).map(function (table) {
        var rows = qsa('tr', table).slice(0, 60).map(function (row) {
          return qsa('th, td', row).map(function (cell) { return clip(cell.innerText || '', 160); });
        });
        return { caption: clip(table.caption ? table.caption.innerText : '', 120), rows: rows };
      }) };
    case 'forms':
      return { ok: true, data: formFields(selector || null, false) };
    case 'metadata':
      var meta = {};
      qsa('meta').forEach(function (el) {
        var key = el.getAttribute('name') || el.getAttribute('property') ||
                  el.getAttribute('http-equiv');
        if (key && el.content) meta[key] = clip(el.content, 400);
      });
      var canonical = qsa('link[rel="canonical"]')[0];
      return { ok: true, data: {
        title: document.title,
        url: location.href,
        canonical: canonical ? canonical.href : '',
        description: meta['description'] || meta['og:description'] || '',
        meta: meta,
        headings: qsa('h1, h2, h3').slice(0, 25).map(function (h) {
          return { level: h.tagName.toLowerCase(), text: clip(h.innerText, 160) };
        }),
        jsonLd: qsa('script[type="application/ld+json"]').slice(0, 5).map(function (el) {
          return clip(el.textContent, 1500);
        })
      } };
    case 'images':
      return { ok: true, data: qsa('img', scope).slice(0, max).map(function (el) {
        return { alt: clip(el.getAttribute('alt') || '', 120), src: el.currentSrc || el.src };
      }) };
    case 'buttons':
      return { ok: true, data: qsa('button, input[type="submit"], input[type="button"], ' +
                                   '[role="button"]', scope).slice(0, max).map(function (el, i) {
        return describe(el, i + 1);
      }) };
    case 'headings':
      return { ok: true, data: qsa('h1, h2, h3, h4, h5, h6', scope).slice(0, max).map(function (el) {
        return { level: el.tagName.toLowerCase(), text: clip(el.innerText, 200) };
      }) };
    case 'html':
      return { ok: true, data: clip(scope.outerHTML || scope.innerHTML || '', 12000) };
    case 'articles':
      return { ok: true, data: qsa('article, main, [role="main"], .post, .article, .content', scope)
        .slice(0, 10).map(function (el) { return clip(el.innerText || '', 2500); }) };
    case 'text':
    default:
      return { ok: true, data: clip(scope.innerText || scope.textContent || '', 4000),
               scope: scopeLabel };
  }
}
// -------------------------------------------------------------- actions ----

function findNearest(target) {
  var wanted = norm(target);
  if (!wanted) return [];
  var words = wanted.split(' ').filter(function (word) { return word.length > 2; });
  var pool = qsa('a, button, [role="button"], input, select, textarea, label, h1, h2, h3');
  var scored = [];
  for (var i = 0; i < pool.length; i++) {
    var el = pool[i];
    if (!isVisible(el)) continue;
    var name = accessibleName(el);
    if (!name) continue;
    var score = 0;
    words.forEach(function (word) { if (name.indexOf(word) !== -1) score += 10; });
    if (score > 0) scored.push({ el: el, score: score });
  }
  scored.sort(function (a, b) { return b.score - a.score; });
  return scored.slice(0, 6).map(function (item, n) { return describe(item.el, n + 1); });
}

function pick(target, index, opts) {
  var scored = resolve(target, opts || {});
  // The caller contract is 1-based ("index 2" = the second match), matching the
  // tab indices and the tool schema. index <= 0 means "no preference": take the
  // best-ranked match. Positions reported back are always 1-based too, so the
  // model can repeat a position it read in `alternatives`/`find`.
  var wanted = (index && index > 0) ? index : 1;
  var position = wanted - 1;
  if (!scored.length) {
    return { ok: false, error: 'no element matched "' + target + '"',
             nearest: findNearest(target) };
  }
  if (position >= scored.length) {
    return { ok: false, error: 'only ' + scored.length + ' element(s) matched "' + target +
                               '" - index ' + wanted + ' does not exist',
             nearest: scored.slice(0, 5).map(function (item, n) {
               return describe(item.el, n + 1);
             }) };
  }
  return { ok: true, element: scored[position].el, total: scored.length, position: wanted,
           alternatives: scored.slice(0, 6).map(function (item, n) {
             return describe(item.el, n + 1);
           }) };
}

// Resolve -> scroll into view -> focus, then report the *fresh* geometry so the
// caller can dispatch real, trusted mouse events at the right coordinates.
function prepareAction(target, index, opts) {
  var chosen = pick(target, index, opts);
  if (!chosen.ok) return chosen;
  scrollIntoView(chosen.element);
  focusEl(chosen.element);
  return { ok: true, data: describe(chosen.element, chosen.position),
           total: chosen.total, alternatives: chosen.alternatives };
}

function readTarget(target, index, opts) {
  var chosen = pick(target, index, opts);
  if (!chosen.ok) return chosen;
  var el = chosen.element;
  return { ok: true, data: { value: currentValue(el), checked: !!el.checked,
                             selector: cssPath(el), tag: el.tagName.toLowerCase(),
                             type: (el.getAttribute('type') || '').toLowerCase() } };
}

function fillTarget(target, index, value, opts) {
  var chosen = pick(target, index, opts);
  if (!chosen.ok) return chosen;
  scrollIntoView(chosen.element);
  var applied = setValue(chosen.element, value);
  return { ok: !!applied,
           data: { selector: cssPath(chosen.element), value: currentValue(chosen.element),
                   applied: applied, label: clip(rawAccessibleName(chosen.element), 80) },
           alternatives: chosen.alternatives };
}

// Click through the page's own event pipeline (used as a fallback when trusted
// mouse dispatch cannot be used, e.g. an overlay swallows the coordinates).
function hoverElement(target, index) {
  var chosen = pick(target, index, {});
  if (!chosen.ok) return chosen;
  var el = chosen.element;
  scrollIntoView(el);
  try {
    var event = new MouseEvent('mouseover', { bubbles: true, cancelable: true, view: window });
    el.dispatchEvent(event);
    return { ok: true, data: { selector: cssPath(el), label: clip(rawAccessibleName(el), 80) },
             alternatives: chosen.alternatives };
  } catch (e) {
    return { ok: false, error: 'hover failed: ' + String(e && e.message || e), alternatives: chosen.alternatives };
  }
}

function clickForce(target, index, opts) {
  var chosen = pick(target, index, opts);
  if (!chosen.ok) return chosen;
  var el = chosen.element;
  scrollIntoView(el);
  try {
    el.click();
  } catch (e) {
    try {
      var event = new MouseEvent('click', { bubbles: true, cancelable: true, view: window });
      el.dispatchEvent(event);
    } catch (e2) {
      return { ok: false, error: 'element could not be clicked: ' + e2 };
    }
  }
  return { ok: true, data: describe(el, chosen.position) };
}

function submitViaForm(target, index, opts) {
  var chosen = pick(target, index, opts);
  if (!chosen.ok) return chosen;
  var form = formOf(chosen.element);
  if (!form) return { ok: false, error: 'element is not inside a form' };
  try {
    if (typeof form.requestSubmit === 'function') {
      form.requestSubmit();
      return { ok: true, data: { method: 'requestSubmit' } };
    }
    form.submit();
    return { ok: true, data: { method: 'submit' } };
  } catch (e) {
    return { ok: false, error: 'form submission failed: ' + (e && e.message ? e.message : e) };
  }
}

function scrollPage(direction, amount) {
  var step = amount && amount > 0 ? amount : Math.round(window.innerHeight * 0.8);
  var vertical = direction === 'up' ? -step : step;
  if (direction === 'left' || direction === 'right') {
    window.scrollBy({ left: direction === 'left' ? -step : step, top: 0, behavior: 'instant' });
  } else if (direction === 'top') {
    window.scrollTo({ top: 0, behavior: 'instant' });
  } else if (direction === 'bottom') {
    window.scrollTo({ top: document.documentElement.scrollHeight, behavior: 'instant' });
  } else {
    window.scrollBy({ top: vertical, left: 0, behavior: 'instant' });
  }
  return { ok: true, data: { scrollY: Math.round(window.scrollY),
                             scrollHeight: document.documentElement.scrollHeight,
                             innerHeight: window.innerHeight },
           atBottom: Math.round(window.scrollY + window.innerHeight) >=
                     document.documentElement.scrollHeight - 4 };
}

function countMatches(text) {
  var wanted = norm(text);
  if (!wanted) return 0;
  var body = norm(document.body ? (document.body.innerText || '') : '');
  var count = 0;
  var cursor = body.indexOf(wanted);
  while (cursor !== -1 && count < 500) {
    count++;
    cursor = body.indexOf(wanted, cursor + wanted.length);
  }
  return count;
}
return {
  version: 1,
  norm: norm,
  isVisible: isVisible,
  isEnabled: isEnabled,
  isFillable: isFillable,
  accessibleName: accessibleName,
  describe: describe,
  cssPath: cssPath,
  pageInfo: pageInfo,
  summary: summary,
  extract: extract,
  findAll: findAll,
  resolve: resolve,
  nearest: findNearest,
  pick: pick,
  prepareAction: prepareAction,
  readTarget: readTarget,
  fillTarget: fillTarget,
  hoverElement: hoverElement,
  clickForce: clickForce,
  submitViaForm: submitViaForm,
  submitControls: submitControls,
  formFields: formFields,
  classify: classify,
  setValue: setValue,
  setChecked: setChecked,
  selectOption: selectOption,
  focusEl: focusEl,
  scrollIntoView: scrollIntoView,
  scrollPage: scrollPage,
  countMatches: countMatches,
  formOf: formOf,
  currentValue: currentValue,
  autofill: autofill
};
)JS";
}  // anonymous namespace

const std::string& runtime_js(const std::string& synonyms_json) {
    // Cached because the synonym table is stable for the whole session.
    static std::string cached;
    static std::string cached_for;
    if (cached.empty() || cached_for != synonyms_json) {
        cached = kRuntimeJsPart1;
        cached += kRuntimeJsPart1b;
        cached += kRuntimeJsPart2;
        cached += kRuntimeJsPart3;
        cached += kRuntimeJsPart4;
        const std::string placeholder = "__SONNY_SYNONYMS__";
        size_t position = cached.find(placeholder);
        if (position != std::string::npos) {
            cached.replace(position, placeholder.size(), synonyms_json);
        } else {
            cached = "var SYNONYMS = " + synonyms_json + ";\n" + cached;
        }
        cached_for = synonyms_json;
    }
    return cached;
}

std::string json_string(const std::string& value) {
    // nlohmann produces a fully escaped, spec-compliant JS string literal.
    return nlohmann::json(value).dump();
}

std::string wrap(const std::string& action_body, const std::string& synonyms_json) {
    std::string script;
    script.reserve(4096 + action_body.size() + synonyms_json.size());
    script += "(function(){\n";
    script += "var API = (function(){\n";
    script += runtime_js(synonyms_json);
    script += "\n})();\n";
    script += "try{\n";
    script += action_body;
    script += "\n}catch(e){\n";
    script += "return JSON.stringify({ok:false,error:String((e&&e.message)||e)});\n";
    script += "}\n})()";
    return script;
}

std::string find_body(const std::string& target_json, const std::string& options_json) {
    return "return JSON.stringify({ok:true, data: API.findAll(" + target_json + ", " +
           options_json + ")});";
}

std::string summary_body() {
    return "return JSON.stringify({ok:true, data: API.summary()});";
}

std::string fields_body(const std::string& scope_json, bool include_hidden) {
    return "return JSON.stringify({ok:true, data: API.formFields(" + scope_json + ", " +
           (include_hidden ? "true" : "false") + ")});";
}

std::string extract_body(const std::string& kind_json, const std::string& selector_json,
                         int limit) {
    return "return JSON.stringify(API.extract(" + kind_json + ", " + selector_json + ", " +
           std::to_string(limit > 0 ? limit : 40) + "));";
}

std::string install_runtime(const std::string& synonyms_json, const std::string& signature) {
    // Installs the API once per document and stamps the fingerprint onto <html>
    // so the service can probe "is the runtime live and current?" with a single
    // attribute read instead of re-sending the whole bundle.
    std::string script;
    script.reserve(1024 + synonyms_json.size());
    script += "(function(){";
    script += "window.__sonny=(function(){";
    script += runtime_js(synonyms_json);
    script += "\n})();";
    script += "var el=document.documentElement;";
    script += "if(el){el.setAttribute('data-sonny-runtime','";
    script += signature;
    script += "');}";
    script += "})()";
    return script;
}

std::string read_body(const std::string& target_json, const std::string& index_json) {
    return "return JSON.stringify(API.readTarget(" + target_json + ", " + index_json +
           ", null));";
}

std::string scroll_body(const std::string& direction_json, const std::string& amount_json) {
    return "return JSON.stringify(API.scrollPage(" + direction_json + ", " + amount_json +
           "));";
}

std::string prepare_body(const std::string& target_json, const std::string& index_json) {
    return "return JSON.stringify(API.prepareAction(" + target_json + ", " + index_json +
           ", null));";
}

std::string click_body(const std::string& target_json, const std::string& index_json,
                       const std::string& options_json) {
    return "return JSON.stringify(API.clickForce(" + target_json + ", " + index_json + ", " +
           options_json + "));";
}

std::string fill_body(const std::string& target_json, const std::string& index_json,
                      const std::string& value_json) {
    return "return JSON.stringify(API.fillTarget(" + target_json + ", " + index_json + ", " +
           value_json + ", null));";
}

std::string hover_body(const std::string& target_json, const std::string& index_json) {
    return "return JSON.stringify(API.hoverElement(" + target_json + ", " + index_json + "));";
}

std::string submit_body(const std::string& target_json, const std::string& index_json) {
    return "return JSON.stringify(API.submitViaForm(" + target_json + ", " + index_json +
           ", null));";
}

std::string autofill_body(const std::string& values_json) {
    return "return JSON.stringify({ok:true, data: API.autofill(" + values_json + ")});";
}


}  // namespace BrowserPageScripts
}  // namespace Browser
}  // namespace Jarvis
// __SONNY_CHUNK_END__
