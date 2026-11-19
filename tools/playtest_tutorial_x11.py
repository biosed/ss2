"""Play the first tutorial objective with ordinary X11 keyboard and mouse input.

Run on a dedicated X display. This script focuses its own window, drives the opening
briefings, clicks the chosen destination and requires both completion messages.
"""
import argparse
import hashlib
import json
import os
import re
from pathlib import Path
import subprocess
import time

POINTS = ((1618, 2321), (1649, 2338))
DESTINATIONS = {
    'ground': ((2592, 1856), (2624, 1872)),
    'wreck': ((2960, 1776), (2960, 1776)),
    'wreck-body': ((2960, 1760), (2960, 1760)),
}


def play(args):
    args.output.mkdir(parents=True, exist_ok=False)
    log_path = args.output / 'game.log'
    engine = args.engine.resolve()
    engine_digest = hashlib.sha256(engine.read_bytes()).hexdigest()
    cli = ['stdbuf', '-oL', '-eL', str(engine), '--windowed',
           '--game-dir', str(args.game.resolve()), '--validate']
    if args.hd:
        cli.extend(['--hd-pack', str(args.hd.resolve())])
    if args.original:
        cli.append('--original-infantry')
    result = {'scenario': args.scenario, 'selection': args.selection, 'hd': bool(args.hd),
              'original': args.original, 'window': args.size, 'camera_controls': args.camera_controls, 'redirect': args.redirect, 'regroup': args.regroup, 'marker_navigation': args.marker_navigation, 'pause_order': args.pause_order, 'fields': args.fields, 'passed': False}
    start = time.monotonic()
    def xdo(*cmd, check=True):
        return subprocess.run(['xdotool', *map(str, cmd)], check=check,
                              capture_output=True, text=True)
    with log_path.open('w') as output, subprocess.Popen(cli, stdout=output,
            stderr=subprocess.STDOUT, env={**os.environ, 'SDL_VIDEO_DRIVER': 'x11'}) as game:
        window = None
        def until(predicate, timeout, action=None):
            deadline = time.monotonic() + timeout
            while time.monotonic() < deadline:
                if predicate():
                    return True
                if game.poll() is not None:
                    raise RuntimeError(f'Game exited with code {game.returncode}')
                if action:
                    action()
                time.sleep(0.16)
            return False
        def visible():
            nonlocal window
            ids = xdo('search', '--onlyvisible', '--pid', game.pid, check=False).stdout.splitlines()
            window = ids[0] if ids else None
            return bool(window)
        def snapshot(name):
            subprocess.run(['magick', 'import', '-window', window, str(args.output/(name+'.png'))],
                           check=True, timeout=12)
        def title():
            return xdo('getwindowname', window).stdout.strip()
        def click(button, xy):
            xdo('mousemove', '--window', window, *xy, 'click', button)
        try:
            if not until(visible, 30):
                raise RuntimeError('Game window did not appear')
            # binfmt_misc can expose the architecture interpreter as the process executable.
            process_exe = Path(f'/proc/{game.pid}/exe')
            result['engine_sha256'] = engine_digest
            result['process_executable_sha256'] = hashlib.sha256(process_exe.read_bytes()).hexdigest()
            result['process_executable'] = process_exe.readlink().name
            width, height = (int(part) for part in args.size.split('x'))
            xdo('windowmove', window, 0, 0, 'windowsize', '--sync', window, width, height)
            xdo('windowfocus', '--sync', window)
            scale = min(width/8192, height/4096)
            pan_x = 0
            center_x, center_y = 4096,2048
            def screen(point):
                x, y = point
                return round(width/2 + (x-center_x)*scale + pan_x), round(height/2 + (y-center_y)*scale)
            reminder_seen = False
            def dismiss_reminder():
                nonlocal reminder_seen, center_x, center_y, scale, pan_x
                if reminder_seen or 'Tutorial I phrase 22\n' not in log_path.read_text():
                    return False
                # Gold opens this modal at tick 3203 if the objective is pending.
                # At accelerated test speed it can arrive between two mouse orders.
                xdo('key','Return')
                reminder_seen = True
                result['reminder_acknowledged_before_order'] = True
                time.sleep(0.18)
                # This script event also moves the camera. Reacquire a known view.
                xdo('key','Home')
                center_x, center_y, scale, pan_x = 4096,2048,min(width/8192,height/4096),0
                time.sleep(0.2)
                return True

            def move_order(point):
                dismiss_reminder()
                offset = log_path.stat().st_size
                def dispatched():
                    with log_path.open('rb') as log:
                        log.seek(offset)
                        data = log.read()
                    return any(event in data for event in
                               (b'accepted move',b'rejected move',b'unsupported attack order'))
                click(3,screen(point))
                if not until(dispatched,0.6) and dismiss_reminder():
                    click(3,screen(point))

            if not until(lambda: 'Tutorial I phrase 29\n' in log_path.read_text(), 90,
                         lambda: xdo('key', 'Return')):
                raise RuntimeError('Opening briefings did not reach the order instructions')
            for _ in range(3):
                xdo('key', 'Return')
                time.sleep(0.2)
            xdo('key', 'Home')
            time.sleep(0.35)
            if args.pause_order:
                xdo('key','Pause')
                if not until(lambda: 'Mission clock paused' in log_path.read_text(), 3):
                    raise RuntimeError('Pause did not stop the mission clock')
            snapshot('before-orders')
            result['orders'] = []
            targets = DESTINATIONS[args.scenario]
            expected = list(targets[:1] if args.selection == 'squad' else targets)
            counts = [29 if args.selection == 'squad' else 1]*len(expected)
            if args.selection == 'squad':
                xdo('mousemove', '--window', window, *screen((1420,2250)), 'mousedown', 1,
                    'mousemove', '--window', window, *screen((1715,2460)), 'mouseup', 1)
                time.sleep(0.15)
                if args.regroup:
                    xdo('key','ctrl+2')
                    time.sleep(0.7)
                    xdo('mousemove','--window',window,*screen(POINTS[0]),'click','--repeat',2,'--delay',100,1)
                    if not until(lambda: '8 infantry selected' in title(), 2):
                        raise RuntimeError('Double-click did not select the eight starting submachine gunners')
                    counts[0] = 8
                if args.camera_controls:
                    xdo('key', 'ctrl+2')
                    click(1, screen((2200, 2200)))
                    xdo('key', '2')
                    xdo('click', '--repeat', 3, 4)
                    scale *= 1.25**3
                    time.sleep(0.3)
                    # Pressing right during a left selection cancels the box and preserves the squad.
                    xdo('mousemove', '--window', window, width//2, height//2, 'mousedown', 1,
                        'mousemove_relative', 32, 24, 'click', 3, 'mouseup', 1)
                    if not until(lambda: '29 infantry selected' in title(), 2):
                        raise RuntimeError('Canceling the left drag lost the squad selection')
                    # A middle drag pans the map.
                    pan_x = 64
                    xdo('mousemove', '--window', window, width//2, height//2, 'mousedown', 2,
                        'mousemove', '--window', window, width//2+pan_x, height//2, 'mouseup', 2)
                    time.sleep(0.3)
                if args.marker_navigation:
                    xdo('click','--repeat',4,4)
                    scale *= 1.25**4
                    xdo('key','Tab')
                    time.sleep(0.3)
                    snapshot('squad-focus')
                    xdo('key','m')
                    time.sleep(0.3)
                    # Native mission VM emitted original marker 1, tile (37,74).
                    center_x, center_y = 2912,1792
                    snapshot('marker-focus')
                selected = title()
                if args.picking:
                    xdo('key', 'ctrl+8')
                    xdo('key', 'Tab')
                    time.sleep(0.3)
                    scale = 2 if args.hd else 1
                    # Tab uses the integer centroid of the 29 initial foot coordinates.
                    center_x = (46096*scale//29)/scale
                    center_y = (68800*scale//29)/scale
                    click(1, screen((1478,2355)))
                    if not until(lambda: 'cmedic' in title(), 2):
                        raise RuntimeError('Picking a medic and gunner at equal distance lost live-list priority')
                    result['tie_target'] = title()
                    xdo('key', '8')
                    xdo('key', 'm')
                    time.sleep(0.4)
                    center_x, center_y, scale = 2912,1792,2 if args.hd else 1
                    # The southern group 2 soldier stands at (2768,1840).
                    # Both pixels are below its painted figure. Only the first is within reach.
                    inside, outside = (2768,1857),(2768,1858)
                    click(1, screen(inside))
                    if not until(lambda: 'Group 2' in title() and 'Owner 1' in title(), 2):
                        raise RuntimeError('Picking at the edge of the infantry radius failed')
                    click(1, screen(outside))
                    if not until(lambda: title() == 'Sudden Strike II | Tutorial I', 2):
                        raise RuntimeError('Clicked ground outside the radius selected a unit')
                    xdo('key', '8')
                    if not until(lambda: '29 infantry selected' in title(), 2):
                        raise RuntimeError('Could not recall the squad after inspecting the enemy')
                    click(3, screen(inside))
                    if not until(lambda: 'unsupported attack order' in log_path.read_text(), 2):
                        raise RuntimeError('Right-click did not acquire the same nearby enemy')
                    # Gold dispatches immediately at the press, even if release is over an enemy.
                    xdo('mousemove', '--window', window, *screen(outside), 'mousedown', 3)
                    try:
                        if not until(lambda: 'accepted move to map pixel 2768,1858 for 29/29' in log_path.read_text(), 3):
                            raise RuntimeError('The ground order did not dispatch while the right button was held')
                        xdo('mousemove_relative', 32, 12, 'mousemove', '--window', window, *screen(inside))
                    finally:
                        xdo('mouseup', 3)
                    time.sleep(0.15)
                    if log_path.read_text().count('unsupported attack order') != 1:
                        raise RuntimeError('Releasing over a soldier issued a second order')
                    result['order_registered_before_release'] = True
                    result['orders'].append([selected,outside,title()])
                    expected.insert(0,outside)
                    counts.insert(0,29)
                    result['checked_pick_pixels'] = [inside,outside]
                if args.redirect:
                    waypoint = (1984,2160)  # Clear approach outside location 2, tile (34,100)
                    move_order(waypoint)
                    time.sleep(0.8)
                    result['orders'].append([selected, waypoint, title()])
                    expected.insert(0,waypoint)
                    counts.insert(0,29)
                move_order(targets[0]); time.sleep(0.15)
                result['orders'].append([selected, targets[0], title()])
            else:
                for soldier, target in zip(POINTS, targets):
                    click(1, screen(soldier)); time.sleep(0.15)
                    selected = title()
                    move_order(target); time.sleep(0.15)
                    result['orders'].append([selected, target, title()])
            snapshot('after-orders')
            if args.pause_order:
                def map_digest():
                    pixels = subprocess.check_output(['magick','import','-window',window,'-crop',
                        f'{width}x{height*3//4}+0+0','-depth','8','rgba:-'],timeout=15)
                    return hashlib.sha256(pixels).hexdigest()
                frozen = map_digest()
                time.sleep(1.2)
                if map_digest() != frozen:
                    raise RuntimeError('Map changed after an order while paused')
                xdo('key','ctrl+p')
                if not until(lambda: 'Mission clock resumed' in log_path.read_text(), 3):
                    raise RuntimeError('Ctrl+P did not resume the mission clock')
                result['map_frozen_while_paused'] = True
            # Continue acknowledging messages during the walk. Both must come from the mission VM.
            captured_success = False
            def next_briefing():
                nonlocal captured_success
                text = log_path.read_text()
                if 'Tutorial I phrase 5\n' in text:
                    return True
                if not captured_success and 'Tutorial I phrase 4\n' in text:
                    snapshot('objective-complete')
                    captured_success = True
                return False
            completed = until(next_briefing, args.walk_timeout, lambda: xdo('key', 'Return'))
            if completed:
                snapshot('next-briefing')
                if args.probe_attack or args.regroup or args.fields:
                    for _ in range(3):
                        xdo('key', 'Return')
                        time.sleep(0.18)
                    xdo('key', 'Home'); time.sleep(0.3)
                    scale, pan_x = min(width/8192, height/4096), 0
                    center_x, center_y = 4096,2048
                if args.regroup:
                    # The forward detachment and remaining squad are now widely dispersed.
                    xdo('key','2')
                    time.sleep(0.2)
                    selected = title()
                    move_order(targets[0])
                    time.sleep(0.3)
                    result['orders'].append([selected,targets[0],title()])
                    expected.append(targets[0])
                    counts.append(29)
                    snapshot('regroup-order')
                enemy = (2768,1827)
                if args.fields:
                    xdo('key', 'ctrl+7')
                    enemy = (3712,1513)
                    click(1, screen(enemy))
                    if not until(lambda: 'Group 3' in title() and 'Owner 1' in title(), 2):
                        raise RuntimeError('Could not inspect a group 3 enemy in the fields')
                    result['field_enemy_title'] = title()
                    xdo('key', '7')
                    if not until(lambda: '29 infantry selected' in title(), 2):
                        raise RuntimeError('Could not recall the squad')
                    field = (3600,1600)
                    selected = title()
                    move_order(field)
                    result['orders'].append([selected,field,title()])
                    expected.append(field)
                    counts.append(29)
                    click(1, screen((4096,100)))
                    if not until(lambda: title() == 'Sudden Strike II | Tutorial I', 2):
                        raise RuntimeError('Could not clear selection before measuring arrival')
                    # Box selection reads live player positions. This rectangle excludes the wreck.
                    def squad_at_fields():
                        xdo('mousemove', '--window', window, *screen((3380,1460)), 'mousedown', 1,
                            'mousemove', '--window', window, *screen((3770,1728)), 'mouseup', 1)
                        time.sleep(0.25)
                        return '29 infantry selected' in title()
                    if not until(squad_at_fields, args.walk_timeout):
                        snapshot('fields-timeout')
                        raise RuntimeError('Selection in the fields at timeout: ' + title())
                    result['infantry_at_fields'] = 29
                    snapshot('squad-at-fields')
                if args.probe_attack or args.fields:
                    previous_attacks = log_path.read_text().count('unsupported attack order')
                    click(3, screen(enemy))
                    result['attack_reported_unsupported'] = until(
                        lambda: log_path.read_text().count('unsupported attack order') > previous_attacks, 3)
                    snapshot('attack-order')
                    if args.fields:
                        # An installed-speed mission scans triggers every 64 ticks (about 2.3 s).
                        if until(lambda: 'Tutorial I phrase 6\n' in log_path.read_text(), 3):
                            raise RuntimeError('The fields objective advanced without combat')
                        click(1, screen(enemy))
                        if not until(lambda: title() == result['field_enemy_title'], 2):
                            raise RuntimeError('The inspected enemy changed after the unsupported order')
            else:
                snapshot('timeout')
            xdo('key', 'Return'); time.sleep(0.18)
            xdo('key', 'Escape')
            game.wait(timeout=12)
            events = log_path.read_text().splitlines()
            result['phrases'] = [int(s.split()[-1]) for s in events if s.startswith('Tutorial I phrase ')]
            result['accepted'] = [s for s in events if 'accepted move' in s]
            events_match = len(result['accepted']) == len(expected)
            for target, count, line in zip(expected, counts, result['accepted']):
                match = re.search(r'pixel (\d+),(\d+) for (\d+)/(\d+) selected', line)
                if not match:
                    events_match = False
                    continue
                x, y, accepted, selected = map(int, match.groups())
                events_match &= (max(abs(x-target[0]), abs(y-target[1])) <= 8
                                 and accepted == selected == count)
            result['passed'] = (events_match and completed and 4 in result['phrases'] and
                                (not (args.probe_attack or args.fields) or result.get('attack_reported_unsupported')) and
                                (not args.fields or 6 not in result['phrases']) and
                                'Validation errors: 0' in events and game.returncode == 0)
        except Exception as error:
            result['error'] = str(error)
            raise
        finally:
            if game.poll() is None:
                game.terminate()
                game.wait(timeout=10)
            result['elapsed_seconds'] = round(time.monotonic()-start, 1)
            (args.output/'result.json').write_text(json.dumps(result, indent=2)+'\n')
    return result


if __name__ == '__main__':
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('--engine', type=Path, default=Path('build/ss2'))
    p.add_argument('--game', type=Path, required=True)
    p.add_argument('--hd', type=Path)
    p.add_argument('--size', choices=['1280x720','1440x900','1600x900','1920x1080','2419x1360','3840x2160'],
                   default='1280x720')
    p.add_argument('--original', action='store_true')
    p.add_argument('--scenario', choices=DESTINATIONS, required=True)
    p.add_argument('--selection', choices=['single', 'squad'], default='single')
    p.add_argument('--probe-attack', action='store_true', help='Inspect the first enemy interaction after the objective')
    p.add_argument('--fields', action='store_true', help='March all 29 soldiers to the fields and inspect the group 3 combat objective')
    p.add_argument('--picking', action='store_true', help='Test adjacent ground and soldier click targets near the wreck at close zoom')
    p.add_argument('--marker-navigation', action='store_true', help='Focus the selected squad with Tab and the mission marker with M before clicking')
    p.add_argument('--pause-order', action='store_true', help='Order while paused, compare the visible map, then resume')
    p.add_argument('--regroup', action='store_true', help='Send the submachine gunners ahead, then recall and order the dispersed 29-soldier squad')
    p.add_argument('--redirect', action='store_true', help='Redirect the moving squad from an intermediate target outside the objective')
    p.add_argument('--camera-controls', action='store_true', help='Recall a control group, zoom and middle-drag before ordering the squad')
    p.add_argument('--walk-timeout', type=float, default=125)
    p.add_argument('--output', type=Path, required=True)
    args = p.parse_args()
    if args.fields and args.selection != 'squad':
        p.error('--fields requires --selection squad')
    if args.picking and (args.selection != 'squad' or args.camera_controls or args.redirect or args.regroup or args.marker_navigation):
        p.error('--picking requires a squad and uses its own camera and orders')
    if args.marker_navigation and (args.camera_controls or args.redirect or args.regroup):
        p.error('--marker-navigation plays its own camera sequence')
    if args.regroup and (args.camera_controls or args.redirect):
        p.error('--regroup plays its own camera and movement sequence')
    if (args.camera_controls or args.redirect or args.regroup or args.marker_navigation) and args.selection != 'squad':
        p.error('--camera-controls and --redirect require --selection squad')
    result = play(args)
    print(json.dumps(result))
    raise SystemExit(0 if result['passed'] else 1)
