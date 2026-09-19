# -*- coding: utf-8 -*-
"""airplay-esp32 (ESP32-S3 N16R8 + PCM5102A) 编译/烧录脚本

手动构造 ESP-IDF 5.5.5 环境，绕过 Git Bash 下 export.bat/ps1 的 MSYS 拦截
和 python 定位问题 (见 memory: esp-idf-build-flash-workaround)。

用法:
    python build_fw.py build
    python build_fw.py flash --port COM3
    python build_fw.py flash-monitor --port COM3
    python build_fw.py monitor --port COM3
    python build_fw.py erase
"""
import glob
import os
import subprocess
import sys

# 项目要求 ESP-IDF >= 5.5 (dependencies.lock:109 锁的正是 5.5.5)；已验证可用。
# 不要换成 6.0 —— 见文末说明。
IDF_PATH = os.environ.get('IDF_PATH', r'C:/esp/v5.5.5/esp-idf')


def _win_to_posix(p):
    # idf.py 和下面的 glob 模式都用正斜杠，Windows 的 expanduser 给反斜杠。
    return p.replace('\\', '/')


# 本脚本就放在项目根目录，所以根目录取自己的位置 —— 这里以前写死过绝对路径，
# 项目被挪动过一次之后报 NotADirectoryError [WinError 267]。别再改回硬编码。
PROJECT = _win_to_posix(os.path.dirname(os.path.abspath(__file__)))
IDF_TOOLS_PATH = _win_to_posix(
    os.environ.get('IDF_TOOLS_PATH') or os.path.join(os.path.expanduser('~'),
                                                     '.espressif'))
PYENV = IDF_TOOLS_PATH + '/python_env/idf5.5_py3.11_env'

# sdkconfig 分层: 基座 + S3 板级 (左到右覆盖)。
# 不加这个，裸 idf.py build 读不到 config/ 里的板级默认值。
SDKCONFIG_DEFAULTS = ('config/sdkconfig.defaults'
                      ';config/sdkconfig.defaults.esp32s3')
# 本地覆盖（.gitignore 里的 config/sdkconfig.user.*）追加在最后 ——
# defaults 链是左到右覆盖，所以它说了算。热点真实口令就在这里，
# 不能进公开仓库。只在文件存在时才加：idf.py 摊到不存在的 defaults
# 会直接 abort，那会让干净的克隆编不过。
USER_DEFAULTS = 'config/sdkconfig.user.esp32s3'
if os.path.exists(PROJECT + '/' + USER_DEFAULTS):
    SDKCONFIG_DEFAULTS += ';' + USER_DEFAULTS

DEFAULT_PORT = 'COM3'

env = {k: v for k, v in os.environ.items()
       if k not in ('MSYSTEM', 'MSYSTEM_PREFIX', 'MSYSTEM_CARCH',
                    'MSYSTEM_CHOST', 'SHELL', 'TERM')}
env['IDF_PATH'] = IDF_PATH
env['ESP_IDF_VERSION'] = '5.5.5'
env['IDF_TOOLS_PATH'] = IDF_TOOLS_PATH
env['IDF_PYTHON_ENV_PATH'] = PYENV
env['ESP_ROM_ELF_DIR'] = IDF_TOOLS_PATH + '/tools/esp-rom-elfs/20241011'
env['IDF_PYTHON_CHECK_CONSTRAINTS'] = '0'

# 工具链版本号随安装变化，这里按前缀探测实际目录，别硬编码版本。
def _tool_dir(pattern):
    hits = sorted(glob.glob(IDF_TOOLS_PATH + '/tools/' + pattern))
    return hits[-1] if hits else None

# IDF 5.5 用 gcc 14.2；若探到 15.x 说明是 6.0 的，编译会出问题。
xtensa = _tool_dir('xtensa-esp-elf/esp-14.*/xtensa-esp-elf/bin') \
    or _tool_dir('xtensa-esp-elf/*/xtensa-esp-elf/bin')
cmake = _tool_dir('cmake/3.*/bin') or _tool_dir('cmake/*/bin')
ninja = _tool_dir('ninja/*')
py = PYENV + '/Scripts'
tools = IDF_PATH + '/tools'
idf_exe = _tool_dir('idf-exe/*')

for _name, _p in (('xtensa', xtensa), ('cmake', cmake),
                  ('ninja', ninja), ('idf-exe', idf_exe)):
    if not _p:
        sys.exit('ERROR: 找不到 %s 工具目录，检查 %s/tools'
                 % (_name, IDF_TOOLS_PATH))

env['PATH'] = ';'.join([xtensa, cmake, ninja, py, tools, idf_exe,
                        env.get('PATH', '')])

idf_py = py + '/python.exe'

port = DEFAULT_PORT
extra = []
i = 0
argv = sys.argv[1:]
while i < len(argv):
    a = argv[i]
    if a == '--port':
        port = argv[i + 1]
        i += 2
        continue
    extra.append(a)
    i += 1

action = extra[0] if extra else 'build'
rest = extra[1:]

idf = IDF_PATH + '/tools/idf.py'
_d = ['-D', 'SDKCONFIG_DEFAULTS=' + SDKCONFIG_DEFAULTS]

if action == 'build':
    cmd = [idf_py, idf] + _d + ['build']
elif action == 'set-target':
    cmd = [idf_py, idf] + _d + ['set-target'] + (rest or ['esp32s3'])
elif action in ('flash', 'flash-monitor', 'monitor', 'erase', 'menuconfig',
                'clean', 'fullclean', 'size', 'reconfigure'):
    cmd = [idf_py, idf, '-p', port] + _d + [action] + rest
else:
    cmd = [idf_py, idf] + extra

print('IDF :', IDF_PATH)
print('CMD :', ' '.join(cmd))
print('CWD :', PROJECT)
sys.stdout.flush()

r = subprocess.run(['cmd.exe', '/c'] + cmd, cwd=PROJECT,
                   capture_output=True, text=True, env=env, timeout=3600)
out = (r.stdout or '') + (r.stderr or '')
lines = out.splitlines()
# 报错行往往在末尾，但提前把 error/FAILED 挑出来，省得翻日志
hits = [l for l in lines if 'error:' in l or 'FAILED:' in l or 'fatal error' in l]
if hits:
    print('--- 错误摘要 ---')
    print('\n'.join(hits[:40]))
    print('--- 日志末尾 ---')
print('\n'.join(lines[-60:]))
sys.exit(r.returncode)
