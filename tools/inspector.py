"""Live FDKss object inspector, property watches and Lua console (standard-library UI)."""
import argparse
from concurrent.futures import ThreadPoolExecutor
import json
import math
from pathlib import Path
import queue
import re
import threading
import tkinter as tk
from tkinter import ttk
from console import execute
from export_battle import capture as capture_battle

ROOTS = ('application', 'framework', 'fieldmap', 'scene', 'player_controller', 'player', 'script_manager', 'player_model')


def quote(value):
    # Protocol identifiers and RTTI filters are ASCII; Lua accepts JSON's ASCII escapes.
    if not value.isascii() or any(ord(ch) < 32 for ch in value):
        raise ValueError('Use printable ASCII in class filters')
    return json.dumps(value)


def schema(game='sky1'):
    if game not in ('sky1', 'sky2'):
        raise ValueError('Unsupported game schema')
    path = Path(__file__).resolve().parents[1] / f'include/fdkss/{game}_properties.inc'
    roots = (*ROOTS, 'actor')
    fields = {root: [('class', False), ('address', False)] for root in roots}
    pattern = re.compile(r'FDKSS_PROPERTY\((\w+), (\w+), .*?, (true|false)\)')
    for match in pattern.finditer(path.read_text(encoding='utf-8')):
        root, name, writable = match.groups()
        fields[root].append((name, writable == 'true'))
    for group in range(28):
        fields[f'script_group:{group}'] = [('class', False), ('address', False), ('group', False),
                                          ('slot_count', False), ('implemented_count', False)]
    return fields


JSON_VALUE = """
local function json_value(value)
    if value == nil then return 'null' end
    if type(value) == 'string' then return string.format('%q', value) end
    if type(value) == 'number' then return string.format('%.17g', value) end
    return tostring(value)
end
"""


class Client:
    def __init__(self, pid, mod):
        self.pid, self.mod = pid, mod
        self.transport_lock = threading.Lock()
        self.original_background = None
        digest = self.query("return fdk.call('game.build_sha256')")
        if digest == '4f84cb3be200cfd0485d9d6b2fec0ba70a63d73d3fb8a29a515e3a7abdbf13cf':
            self.game = 'sky1'
        elif digest == 'd8b2911d1576216bdc22d070550e4f531e105de7ed2981885849669f4acf8aaf':
            self.game = 'sky2'
        else:
            raise RuntimeError('No verified inspector schema for this executable')
        self.fields = schema(self.game)
        self.roots = tuple(root for root in self.fields if root != 'actor')

    def enable_background(self):
        if self.game == 'sky2':
            if self.query('return fdk.engine.framework.run_in_background') != 'true':
                raise RuntimeError('Enable background updates in Sky 2 game settings before opening the inspector')
            return
        previous = self.query("local previous = fdk.engine.framework.run_in_background; fdk.engine.framework.run_in_background = true; return previous")
        self.original_background = previous == 'true'

    def query(self, source):
        with self.transport_lock:
            return execute(self.pid, self.mod, source)

    def battle(self):
        return capture_battle(self.query)

    def close(self):
        if self.original_background is not None:
            try:
                self.query('fdk.engine.framework.run_in_background = ' + str(self.original_background).lower())
            except (OSError, RuntimeError):
                pass  # Adapter stop/detach also restores its own initial value.
            self.original_background = None

    def objects(self, class_filter=''):
        objects = []
        for root in self.roots:
            result = json.loads(self.query(JSON_VALUE + f"""
                local object = fdk.object({quote(root)})
                return '{{"class":' .. json_value(object.class) .. ',"address":' .. json_value(object.address) .. '}}'
            """))
            objects.append({'id': root, **result})
        offset = 0
        while True:
            page = json.loads(self.query(f"""
                local page = table.pack(fdk.invoke('engine.list', {offset}, 32, {quote(class_filter)}))
                local rows = {{}}
                for i = 2, page.n do
                    local object = fdk.object(page[i])
                    rows[#rows + 1] = string.format('{{"id":%q,"class":%q,"address":%q}}',
                        page[i], object.class, object.address)
                end
                return '{{"next":' .. page[1] .. ',"objects":[' .. table.concat(rows, ',') .. ']}}'
            """))
            objects.extend(page['objects'])
            if page['next'] == -1:
                return objects
            if page['next'] <= offset:
                raise RuntimeError('Invalid enumeration cursor')
            offset = page['next']

    def properties(self, identity, type_name=None):
        field_root = identity if identity in self.roots else 'actor'
        fields = list(self.fields[field_root])
        character = identity == 'player' or type_name == '.?AVActor@chara@sora@@'
        if field_root == 'actor' and character:
            fields = list(self.fields['player']) + [field for field in fields if field[0] not in dict(self.fields['player'])]
        if field_root == 'player' and 'actor' in self.fields:
            fields.extend(field for field in self.fields['actor'] if field[0] not in dict(fields))
        names = ','.join(quote(name) for name, _ in fields)
        values = json.loads(self.query(JSON_VALUE + f"""
            local object = fdk.object({quote(identity)})
            local rows, errors = {{}}, {{}}
            for _, name in ipairs({{{names}}}) do
                local ok, value = pcall(function() return object[name] end)
                if not ok then
                    if name == 'class' or name == 'address' then error(value) end
                    errors[#errors + 1] = string.format('%q', name) .. ':' .. json_value(tostring(value))
                    value = nil
                end
                rows[#rows + 1] = string.format('%q', name) .. ':' .. json_value(value)
            end
            return '{{"values":{{' .. table.concat(rows, ',') .. '}},"errors":{{' .. table.concat(errors, ',') .. '}}}}'
        """))
        errors = values['errors']
        return [(name, ('Unavailable: ' + errors[name]) if name in errors else values['values'][name],
                 name not in errors and writable and (identity == 'framework' or character)) for name, writable in fields]

    def set_property(self, identity, name, value):
        number = float(value)
        if not math.isfinite(number):
            raise ValueError('Value must be a finite number')
        if name == 'time_scale' and identity == 'framework':
            return self.set_time_scale(number)
        if name in ('hp','ep','cp'):
            if number != int(number) or not -2147483648 <= number <= 2147483647:
                raise ValueError('Resource value must be a signed 32-bit integer')
            return self.query(f"return fdk.invoke('engine.set', {quote(identity)}, {quote(name)}, {int(number)})")
        if name == 'native_state_byte':
            if number != int(number) or not 0 <= number <= 255:
                raise ValueError('Native state byte must be an integer within 0..255')
            return self.query(f"return fdk.invoke('engine.set', {quote(identity)}, {quote(name)}, {int(number)})")
        if name == 'yaw_radians':
            if abs(number) > 2 * math.pi:
                raise ValueError('Yaw must be within +/-2*pi radians')
            return self.query(f"return fdk.invoke('engine.set', {quote(identity)}, {quote(name)}, {number!r})")
        if name not in ('position_x', 'position_y', 'position_z') or abs(number) > 1000000:
            raise ValueError('Select an editable coordinate; value must be within +/-1000000')
        return self.query(f"return fdk.invoke('engine.set', {quote(identity)}, {quote(name)}, {number!r})")

    def set_time_scale(self, value):
        number = float(value)
        if not math.isfinite(number) or not 0.25 <= number <= 2:
            raise ValueError('Time scale must be between 0.25 and 2')
        return self.query(f"return fdk.invoke('engine.set', 'framework', 'time_scale', {number!r})")

    def mods(self):
        root = Path(self.query("return fdk.call('game.mod_directory')")).resolve(strict=True)
        loaded = set(json.loads(self.query("return fdk.call('game.mod_names')")))
        return [{'name': path.name, 'enabled': (path / 'enabled.txt').is_file(), 'loaded': path.name in loaded}
                for path in sorted(root.iterdir(), key=lambda p: p.name.lower())
                if path.is_dir() and re.fullmatch(r'[A-Za-z0-9_-]+', path.name)]

    def set_mod_enabled(self, name, enabled):
        if not re.fullmatch(r'[A-Za-z0-9_-]+', name):
            raise ValueError('Invalid mod name')
        if name == self.mod and not enabled:
            raise ValueError(f'The console uses {name}; choose another console mod before disabling it')
        root = Path(self.query("return fdk.call('game.mod_directory')")).resolve(strict=True)
        directory = (root / name).resolve(strict=True)
        marker = directory / 'enabled.txt'
        if directory.parent != root or not directory.is_dir() or marker.resolve().parent != directory:
            raise ValueError('Mod marker must remain inside the active Mods directory')
        if enabled:
            marker.write_bytes(b'1\n')
        else:
            marker.unlink(missing_ok=True)
        return self.query("return fdk.call('game.request_reload')")


class Inspector:
    def __init__(self, root, client):
        self.root, self.client = root, client
        self.pool = ThreadPoolExecutor(max_workers=1)
        self.messages = queue.Queue()
        self.closed, self.watch_pending, self.refresh_pending = False, False, False
        self.rows, self.property_rows = {}, []
        root.title(f'FDKss Inspector — PID {client.pid}')
        root.geometry('1080x720')
        root.protocol('WM_DELETE_WINDOW', self.close)
        tabs = ttk.Notebook(root)
        tabs.pack(fill='both', expand=True, padx=8, pady=8)
        objects_tab, console_tab, mods_tab = ttk.Frame(tabs), ttk.Frame(tabs), ttk.Frame(tabs)
        tabs.add(objects_tab, text='Objects and properties')
        tabs.add(console_tab, text='Lua console')
        tabs.add(mods_tab, text='Mods')
        battle_tab=ttk.Frame(tabs);tabs.add(battle_tab,text='Battle')
        self.battle_status=tk.StringVar(value='Refresh to copy the current roster.')
        ttk.Button(battle_tab,text='Refresh roster',command=self.refresh_battle).pack(anchor='w',pady=8)
        ttk.Label(battle_tab,textvariable=self.battle_status).pack(anchor='w')
        fields=('index','status_word','level','hp','max_hp','ep','max_ep','cp','max_cp')
        self.battle_rows=ttk.Treeview(battle_tab,columns=fields,show='headings')
        for field in fields:
            self.battle_rows.heading(field,text=field);self.battle_rows.column(field,width=90)
        self.battle_rows.pack(fill='both',expand=True)
        self.battle_pending=False
        self.battle_snapshot_data=None
        self.mod_rows = {}
        self.mods = ttk.Treeview(mods_tab, columns=('enabled', 'loaded'), show='tree headings')
        self.mods.heading('#0', text='Mod'); self.mods.heading('enabled', text='Enabled'); self.mods.heading('loaded', text='Running')
        self.mods.pack(fill='both', expand=True)
        mod_buttons = ttk.Frame(mods_tab); mod_buttons.pack(fill='x', pady=8)
        ttk.Button(mod_buttons, text='Refresh', command=self.refresh_mods).pack(side='left')
        ttk.Button(mod_buttons, text='Enable selected', command=lambda: self.change_mod(True)).pack(side='left', padx=8)
        ttk.Button(mod_buttons, text='Disable selected', command=lambda: self.change_mod(False)).pack(side='left')
        ttk.Label(mods_tab, text='Changes apply at the next game update. Enabled mods reload; failed reloads retain their running state.').pack(anchor='w')
        toolbar = ttk.Frame(objects_tab)
        toolbar.pack(fill='x', pady=4)
        ttk.Label(toolbar, text='Class filter').pack(side='left')
        self.filter = tk.StringVar()
        ttk.Entry(toolbar, textvariable=self.filter, width=40).pack(side='left', padx=6)
        ttk.Button(toolbar, text='Refresh objects', command=self.refresh).pack(side='left')
        self.watch = tk.BooleanVar(value=True)
        ttk.Checkbutton(toolbar, text='Watch selected properties', variable=self.watch).pack(side='left', padx=12)
        pane = ttk.Panedwindow(objects_tab, orient='horizontal')
        pane.pack(fill='both', expand=True)
        left, right = ttk.Frame(pane), ttk.Frame(pane)
        pane.add(left, weight=3); pane.add(right, weight=2)
        self.objects = ttk.Treeview(left, columns=('class', 'address'), show='tree headings')
        self.objects.heading('#0', text='Root / actor'); self.objects.column('#0', width=130)
        self.objects.heading('class', text='Class'); self.objects.column('class', width=310)
        self.objects.heading('address', text='Address'); self.objects.column('address', width=150)
        self.objects.pack(side='left', fill='both', expand=True)
        scroll = ttk.Scrollbar(left, orient='vertical', command=self.objects.yview)
        scroll.pack(side='right', fill='y'); self.objects.configure(yscrollcommand=scroll.set)
        self.objects.bind('<<TreeviewSelect>>', lambda _: self.read_selected())
        self.properties = ttk.Treeview(right, columns=('value', 'access'), show='tree headings')
        self.properties.heading('#0', text='Property'); self.properties.column('#0', width=120)
        self.properties.heading('value', text='Value'); self.properties.column('value', width=160)
        self.properties.heading('access', text='Access'); self.properties.column('access', width=80)
        self.properties.pack(fill='both', expand=True)
        editor = ttk.Frame(right)
        editor.pack(fill='x', pady=8)
        ttk.Label(editor, text='Selected value').pack(side='left')
        self.value = tk.StringVar(value='1')
        ttk.Entry(editor, textvariable=self.value, width=10).pack(side='left', padx=6)
        ttk.Button(editor, text='Apply', command=self.write_property).pack(side='left')
        ttk.Label(right, text='Select an editable property to change it.\nPosition edits move the character immediately.').pack(anchor='w')
        ttk.Label(console_tab, text=f'Execute in {client.mod}; use return to display values.').pack(anchor='w')
        self.source = tk.Text(console_tab, height=8, wrap='word')
        self.source.pack(fill='x', pady=4)
        self.source.insert('1.0', 'return fdk.engine.scene.map_id, fdk.engine.framework.fps')
        ttk.Button(console_tab, text='Execute', command=self.run_console).pack(anchor='w')
        self.output = tk.Text(console_tab, state='disabled', wrap='word')
        self.output.pack(fill='both', expand=True, pady=8)
        self.status = tk.StringVar(value='Connecting…')
        ttk.Label(root, textvariable=self.status).pack(fill='x', padx=8, pady=4)
        root.after(50, self.deliver)
        root.after(500, self.poll_watch)
        self.refresh()
        self.refresh_mods()

    def submit(self, function, done):
        future = self.pool.submit(function)
        future.add_done_callback(lambda result: self.messages.put((result, done)))

    def refresh_battle(self):
        if self.battle_pending:return
        self.battle_pending=True
        self.battle_snapshot_data=None
        self.battle_rows.delete(*self.battle_rows.get_children())
        self.battle_status.set('Copying current roster…')
        def done(snapshot,error):
            self.battle_pending=False
            if error:
                self.battle_status.set(str(error));return
            self.battle_snapshot_data=snapshot
            for row in snapshot['records']:
                self.battle_rows.insert('', 'end',values=tuple(row[f] for f in self.battle_rows['columns']))
            availability='Available' if snapshot['available'] else 'Unavailable'
            self.battle_status.set(f'{availability}: {snapshot["count"]} copied records at {snapshot["observed_utc"]}; refresh for current values.')
        self.submit(self.client.battle,done)

    def deliver(self):
        while not self.messages.empty():
            future, done = self.messages.get()
            try:
                done(future.result(), None)
            except Exception as error:
                self.status.set(str(error))
                done(None, error)
        if not self.closed:
            self.root.after(50, self.deliver)

    def refresh(self):
        if self.refresh_pending:
            return
        self.refresh_pending = True
        class_filter = self.filter.get()
        self.status.set('Reading current scene objects…')
        def done(rows, error):
            self.refresh_pending = False
            if error:
                return
            selection = self.objects.selection()
            self.objects.delete(*self.objects.get_children())
            self.rows = {row['id']: row for row in rows}
            actor_count = 0
            for row in rows:
                if row['id'] in ROOTS:
                    label = row['id']
                elif row['id'].startswith('script_group:'):
                    label = 'Command group ' + row['id'].split(':', 1)[1]
                else:
                    actor_count += 1
                    label = f'Actor {actor_count}'
                self.objects.insert('', 'end', iid=row['id'], text=label,
                                    values=(row['class'] or 'Unavailable', row['address'] or ''))
            selected = selection[0] if selection and selection[0] in self.rows else 'framework'
            self.objects.selection_set(selected)
            self.status.set(f'{actor_count} scene actors; {len(self.client.roots)} roots')
            self.read_selected()
        self.submit(lambda: self.client.objects(class_filter), done)

    def read_selected(self):
        selection = self.objects.selection()
        if not selection or self.watch_pending:
            return
        identity = selection[0]
        row = self.rows.get(identity)
        if not row:
            return
        self.watch_pending = True
        def done(values, error):
            self.watch_pending = False
            if error or self.objects.selection() != (identity,):
                return
            self.property_rows = values
            selected_property = self.properties.selection()
            self.properties.delete(*self.properties.get_children())
            for name, value, writable in values:
                self.properties.insert('', 'end', iid=name, text=name,
                                       values=('Unavailable' if value is None else value,
                                               'Managed' if name == 'run_in_background' else 'Editable' if writable else 'Read-only'))
            if selected_property and self.properties.exists(selected_property[0]):
                self.properties.selection_set(selected_property[0])
        self.submit(lambda: self.client.properties(identity, row['class']), done)

    def poll_watch(self):
        if self.watch.get() and not self.refresh_pending:
            self.read_selected()
        if not self.closed:
            self.root.after(500, self.poll_watch)

    def write_property(self):
        objects, properties = self.objects.selection(), self.properties.selection()
        if not objects or not properties:
            self.status.set('Select an object and an editable property')
            return
        identity, name = objects[0], properties[0]
        if not any(field == name and writable for field, _, writable in self.property_rows):
            self.status.set('Selected property is read-only')
            return
        value = self.value.get()
        def done(result, error):
            if not error:
                self.status.set(f'{name} set to {result}')
                self.read_selected()
        self.submit(lambda: self.client.set_property(identity, name, value), done)

    def run_console(self):
        source = self.source.get('1.0', 'end-1c')
        def done(result, error):
            self.output.configure(state='normal')
            self.output.insert('end', str(error) if error else result)
            self.output.insert('end', '\n'); self.output.see('end')
            self.output.configure(state='disabled')
        self.submit(lambda: self.client.query(source), done)

    def refresh_mods(self):
        def done(rows, error):
            if error: return
            self.mod_rows = {row['name']: row for row in rows}
            self.mods.delete(*self.mods.get_children())
            for row in rows:
                self.mods.insert('', 'end', iid=row['name'], text=row['name'],
                    values=('Yes' if row['enabled'] else 'No', 'Yes' if row['loaded'] else 'No'))
        self.submit(self.client.mods, done)

    def change_mod(self, enabled):
        selection = self.mods.selection()
        if not selection:
            self.status.set('Select a mod first'); return
        name = selection[0]
        def done(result, error):
            if error: return
            self.status.set(f'{name}: change queued')
            self.root.after(300, self.refresh_mods)
        self.submit(lambda: self.client.set_mod_enabled(name, enabled), done)

    def close(self):
        self.closed = True
        self.pool.shutdown(wait=False, cancel_futures=True)
        self.root.destroy()
        self.client.close()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('pid', type=int)
    parser.add_argument('--mod', default='HelloFDK')
    parser.add_argument('--smoke-test', action='store_true', help='Test UI and live reads with the window hidden')
    parser.add_argument('--battle-smoke-test', action='store_true', help='Also exercise the Battle tab during the hidden smoke test')
    args = parser.parse_args()
    if args.battle_smoke_test and not args.smoke_test:
        parser.error('--battle-smoke-test requires --smoke-test')
    client = Client(args.pid, args.mod)
    try:
        client.enable_background()
    except (OSError, RuntimeError) as error:
        client.close()
        parser.exit(1, f'Cannot enable background updates: {error}\n')
    root = tk.Tk()
    if args.smoke_test:
        root.withdraw()
    app = Inspector(root, client)
    if args.battle_smoke_test:app.refresh_battle()
    if args.smoke_test:
        outcome = {'ok': False}
        def check():
            if app.rows and app.property_rows and app.mod_rows and (not args.battle_smoke_test or app.battle_snapshot_data is not None):
                outcome['ok'] = True
                print(f'Inspector UI/live smoke passed: {len(app.rows)} objects, {len(app.property_rows)} selected properties')
                if args.battle_smoke_test:
                    print(f'Battle tab: {len(app.battle_rows.get_children())} copied rows; available={app.battle_snapshot_data["available"]}')
                app.close()
            elif not app.closed:
                root.after(100, check)
        root.after(100, check)
        root.after(15000, lambda: app.close() if not app.closed else None)
        root.mainloop()
        if not outcome['ok']:
            print('Inspector smoke failed:', app.status.get())
        return 0 if outcome['ok'] else 1
    root.mainloop()
    return 0


if __name__ == '__main__':
    raise SystemExit(main())
