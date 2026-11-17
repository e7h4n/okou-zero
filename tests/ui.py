"""Render the real LVGL UI without display/input hardware; exercise keyboard state."""
import pathlib
import subprocess
import sys
import tempfile

binary = sys.argv[1]
with tempfile.TemporaryDirectory() as folder:
    def render(name, keys):
        path = pathlib.Path(folder) / (name + '.ppm')
        result = subprocess.run([binary, '--headless', '--output', str(path), '--keys', keys], check=True, capture_output=True, text=True)
        data = path.read_bytes()
        header = b'P6\n320 170\n255\n'
        assert data.startswith(header)
        pixels = data[len(header):]
        assert len(pixels) == 320 * 170 * 3
        assert len(set(pixels)) > 20, 'UI must contain more than a blank frame'
        assert pixels[:3] == bytes.fromhex('f8f5f3'), 'Background color drift'
        return data, result.stdout
    baseline, output = render('hello', '')
    assert 'greetings=0 running=1' in output
    repeated, _ = render('repeat', '')
    assert baseline == repeated, 'Rendering must be deterministic'
    changed, output = render('keyboard', 'enter,enter,esc,enter')
    assert changed != baseline
    assert 'greetings=2 running=0' in output
    for args in [['--bad-flag'], ['--keys', 'enter'], ['--headless', '--keys', 'unknown'], ['--headless', '--output', '/no/such/directory/image.ppm']]:
        assert subprocess.run([binary, *args], capture_output=True).returncode != 0
print('LVGL 320x170 render and keyboard tests passed')
