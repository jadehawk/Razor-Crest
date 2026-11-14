'use strict';

const assert = require('node:assert/strict');
const fs = require('node:fs');
const path = require('node:path');
const test = require('node:test');
const vm = require('node:vm');

const root = path.resolve(__dirname, '..');
const hostScript = fs.readFileSync(path.join(root, 'src/network/html/js/plugin-host.js'), 'utf8');

class Element {
  constructor(tag) {
    this.tag = tag;
    this.children = [];
    this.id = '';
    this.style = {};
    this._text = '';
  }
  appendChild(child) { this.children.push(child); return child; }
  replaceChildren(...children) { this.children = children; this._text = ''; }
  set textContent(value) { this._text = String(value); this.children = []; }
  get textContent() { return this._text || this.children.map(child => child.textContent).join(' '); }
}

const installed = [
  {name: 'zeta', title: 'Zeta', hasPluginJs: true, mount: 'settings'},
  {name: 'native', title: 'Native', hasPluginJs: false},
  {name: 'pluginhub', title: 'Plugin Hub', hasPluginJs: true, mount: 'settings'},
  {name: 'transfer', title: 'Transfer', hasPluginJs: true, mount: 'files'}
];

async function load(mount, plugins, {fail = [], late = [], enabled = true, ok = true, onDiscovered} = {}) {
  const host = new Element('div');
  const requests = [];
  const pending = [];
  const document = {
    getElementById: id => id === 'plugin-container' ? host : null,
    createElement: tag => new Element(tag),
    body: {
      appendChild(script) {
        const name = new URL(script.src, 'http://device.test').searchParams.get('name');
        requests.push(name);
        if (late.includes(name)) {
          pending.push({name, script});
          return;
        }
        queueMicrotask(() => {
          if (fail.includes(name)) return script.onerror();
          script.cpRender = container => { container.textContent = 'Rendered ' + name; };
          script.onload();
        });
      }
    }
  };
  const warnings = [];
  const context = {
    document,
    console: {warn: (...args) => warnings.push(args)},
    setTimeout: late.length ? ((callback, _delay, ...args) => setTimeout(() => callback(...args), 0)) : setTimeout,
    clearTimeout,
    encodeURIComponent,
    fetch: async () => ({
      ok,
      headers: {get: () => enabled ? '1' : '0'},
      json: async () => plugins
    })
  };
  context.window = context;
  vm.createContext(context);
  vm.runInContext(hostScript, context, {filename: 'plugin-host.js'});
  let managementCards = 0;
  const renderCard = (container, plugin, hubAvailable) => {
    managementCards++;
    const heading = new Element('h2');
    heading.textContent = 'Management ' + plugin.name;
    const actions = new Element('div');
    actions.textContent = 'Reinstall Remove';
    const panel = new Element('div');
    container.appendChild(heading);
    container.appendChild(actions);
    container.appendChild(panel);
    if (plugin.name === 'pluginhub') assert.equal(hubAvailable, true);
    return panel;
  };
  const options = mount === 'plugins' ? {
    renderCard,
    onDiscovered: (list, state) => {
      if (onDiscovered) onDiscovered(list, state, host, requests);
    }
  } : undefined;
  const result = await context.loadPlugins(mount, null, options);
  return {result, host, requests, pending, managementCards, warnings};
}

test('management page lists all installed plugins before scripts start', async () => {
  let discovered = false;
  const result = await load('plugins', installed.slice(), {
    onDiscovered(list, enabled, host, requests) {
      discovered = true;
      assert.equal(enabled, true);
      assert.equal(list.length, 4);
      assert.equal(host.children.length, 4);
      assert.equal(requests.length, 0);
      assert.equal(host.children[0].id, 'plugin-pluginhub');
    }
  });
  assert.equal(discovered, true);
  assert.equal(result.result, true);
  assert.equal(result.managementCards, 4);
  assert.deepEqual(result.host.children.map(e => e.id),
    ['plugin-pluginhub', 'plugin-native', 'plugin-transfer', 'plugin-zeta']);
  assert.deepEqual(result.requests, ['pluginhub', 'zeta']);
  assert.match(result.host.children[1].textContent, /Management native/);
  assert.match(result.host.children[2].textContent, /File Manager page/);
  assert.match(result.host.children[3].textContent, /Rendered zeta/);
  assert.match(result.host.children[3].textContent, /Reinstall Remove/);
});

test('File Manager and headless runner load only eligible browser scripts', async () => {
  const files = await load('files', installed.slice());
  assert.equal(files.result, true);
  assert.deepEqual(files.requests, ['transfer']);
  assert.equal(files.managementCards, 0);
  const runner = await load(null, installed.slice());
  assert.equal(runner.result, true);
  assert.deepEqual(runner.requests, ['zeta', 'pluginhub', 'transfer']);
  assert.equal(runner.host.children.some(e => e.id === 'plugin-native'), false);
});

test('failed plugin script keeps the management header and actions', async () => {
  const result = await load('plugins', [{name: 'broken', hasPluginJs: true}], {fail: ['broken']});
  assert.equal(result.result, true);
  assert.match(result.host.children[0].textContent, /Management broken/);
  assert.match(result.host.children[0].textContent, /Reinstall Remove/);
  assert.match(result.host.children[0].textContent, /Failed to load plugin/);
});

test('timed-out plugin can render late without removing management controls', async () => {
  const result = await load('plugins', [{name: 'slow', hasPluginJs: true}], {late: ['slow']});
  assert.match(result.host.children[0].textContent, /Failed to load plugin/);
  const script = result.pending[0].script;
  script.cpRender = panel => { panel.textContent = 'Late render'; };
  script.onload();
  await new Promise(resolve => setImmediate(resolve));
  assert.match(result.host.children[0].textContent, /Late render/);
  assert.match(result.host.children[0].textContent, /Reinstall Remove/);
});

test('invalid list and HTTP errors fail cleanly; disabled state is reported', async () => {
  assert.equal((await load('plugins', {error: 'out of memory'})).result, false);
  assert.equal((await load('plugins', installed.slice(), {ok: false})).result, false);
  let reported = true;
  const disabled = await load('plugins', installed.slice(), {
    enabled: false,
    onDiscovered(_list, value) { reported = value; }
  });
  assert.equal(disabled.result, true);
  assert.equal(reported, false);
  assert.equal(disabled.host.children.length, 0);
  assert.equal(disabled.requests.length, 0);
});

test('Plugins page uses shared script and retains management actions and navigation', () => {
  const html = fs.readFileSync(path.join(root, 'src/network/html/PluginsPage.html'), 'utf8');
  assert.match(html, /<script src="\/js\/plugin-host\.js"><\/script>/);
  assert.doesNotMatch(html, /PluginHost\.js\.inc/);
  assert.match(html, /renderCard: renderInstalledPluginCard/);
  assert.match(html, /\/api\/plugins\/remove/);
  assert.match(html, /action=reinstall/);
  assert.match(html, /scrollIntoView\(\{behavior: 'smooth', block: 'start'\}\)/);
  assert.match(html, /#plugin-picker\[hidden\]/);
});
