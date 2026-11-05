"""Um passo no OTC via teclado do Windows, com verificacao externa da posicao."""
import argparse
import ctypes as C
from ctypes import wintypes as W
import json
from pathlib import Path
import struct
import sys
import time

import psutil
from otc_monitor import Monitor
from value_scan import Memory, choose_pid

DIRECTIONS = {'north': (0x26, 0, -1), 'east': (0x27, 1, 0),
              'south': (0x28, 0, 1), 'west': (0x25, -1, 0)}


class KEYBDINPUT(C.Structure):
    _fields_ = [('wVk', W.WORD), ('wScan', W.WORD), ('dwFlags', W.DWORD),
                ('time', W.DWORD), ('dwExtraInfo', C.c_size_t)]


class MOUSEINPUT(C.Structure):
    _fields_ = [('dx', W.LONG), ('dy', W.LONG), ('mouseData', W.DWORD),
                ('dwFlags', W.DWORD), ('time', W.DWORD), ('dwExtraInfo', C.c_size_t)]


class HARDWAREINPUT(C.Structure):
    _fields_ = [('uMsg', W.DWORD), ('wParamL', W.WORD), ('wParamH', W.WORD)]


class INPUTUNION(C.Union):
    _fields_ = [('ki', KEYBDINPUT), ('mi', MOUSEINPUT), ('hi', HARDWAREINPUT)]


class INPUT(C.Structure):
    _fields_ = [('type', W.DWORD), ('data', INPUTUNION)]


def message_lparam(scan, up, extended=False):
    # Repeticao=1; scan code nos bits 16..23; prefixo E0/E1 no bit 24.
    value = 1 | ((scan & 0xFF) << 16)
    if extended or (scan & 0xFF00) in (0xE000, 0xE100):
        value |= 1 << 24
    if up:
        value |= (1 << 30) | (1 << 31)
    return value


class Keyboard:
    def __init__(self, mode='sendinput', target_pid=None):
        self.mode, self.target_pid = mode, target_pid
        self.message_hwnd = None
        self.api = C.WinDLL('user32', use_last_error=True)
        self.api.GetForegroundWindow.argtypes = []
        self.api.GetForegroundWindow.restype = W.HWND
        self.api.GetWindowThreadProcessId.argtypes = [W.HWND, C.POINTER(W.DWORD)]
        self.api.GetWindowThreadProcessId.restype = W.DWORD
        self.api.GetAsyncKeyState.argtypes = [C.c_int]
        self.api.GetAsyncKeyState.restype = W.SHORT
        self.api.SendInput.argtypes = [W.UINT, C.POINTER(INPUT), C.c_int]
        self.api.SendInput.restype = W.UINT
        self.api.PostMessageW.argtypes = [W.HWND, W.UINT, W.WPARAM, W.LPARAM]
        self.api.PostMessageW.restype = W.BOOL
        self.api.MapVirtualKeyW.argtypes = [W.UINT, W.UINT]
        self.api.MapVirtualKeyW.restype = W.UINT

    def window_pid(self, window):
        pid = W.DWORD()
        if window:
            self.api.GetWindowThreadProcessId(window, C.byref(pid))
        return pid.value

    def focused_pid(self):
        return self.window_pid(self.api.GetForegroundWindow())

    def held(self, key):
        return bool(self.api.GetAsyncKeyState(key) & 0x8000)

    def send(self, key, up):
        if self.mode == 'postmessage':
            return self.send_message(key, up)
        event = INPUT()
        event.type = 1
        # Setas sao teclas estendidas; KEYEVENTF_KEYUP solta a tecla.
        event.data.ki = KEYBDINPUT(key, 0, 0x0001 | (0x0002 if up else 0), 0, 0)
        C.set_last_error(0)
        if self.api.SendInput(1, C.byref(event), C.sizeof(INPUT)) != 1:
            raise RuntimeError(f'SendInput nao enviou o evento (WinError {C.get_last_error()}). '
                               'Confira o nivel de permissao do jogo e do terminal.')

    def send_message(self, key, up):
        if not up:
            self.message_hwnd = self.api.GetForegroundWindow()
        window = self.message_hwnd
        if not window or self.window_pid(window) != self.target_pid:
            raise RuntimeError('Janela de destino indisponivel ou pertence a outro processo.')
        scan = self.api.MapVirtualKeyW(key, 4)  # MAPVK_VK_TO_VSC_EX
        if not scan:
            raise RuntimeError('Windows nao retornou scan code para a tecla.')
        message = 0x0101 if up else 0x0100
        # Algumas configuracoes retornam 0x48 sem E0 para VK_UP.
        # VK_LEFT/UP/RIGHT/DOWN representam as setas estendidas, nao o teclado numerico.
        param = message_lparam(scan, up, extended=key in (0x25, 0x26, 0x27, 0x28))
        C.set_last_error(0)
        if not self.api.PostMessageW(window, message, key, param):
            error = C.get_last_error()
            raise RuntimeError(f'PostMessageW falhou: WinError {error}: {C.FormatError(error).strip()}')
        print(f'[POST] hwnd=0x{window:X} msg=0x{message:04X} vk=0x{key:X} '
              f'lParam=0x{param:08X}: mensagem enfileirada', flush=True)


def press_once(keyboard, pid, key, sleeper=time.sleep):
    if keyboard.focused_pid() != pid:
        raise RuntimeError('Antica nao esta em primeiro plano. Nenhuma seta foi enviada.')
    if any(keyboard.held(k) for k in (0x10, 0x11, 0x12, 0x5B, 0x5C, 0x25, 0x26, 0x27, 0x28)):
        raise RuntimeError('Solte setas e teclas modificadoras antes de testar. Nenhuma seta foi enviada.')
    if keyboard.focused_pid() != pid:
        raise RuntimeError('Foco mudou. Nenhuma seta foi enviada.')
    down_sent = False
    try:
        keyboard.send(key, False)
        down_sent = True
        sleeper(0.08)
    finally:
        # Solta mesmo se o foco mudar ou o usuario interromper o teste.
        if down_sent:
            keyboard.send(key, True)


def position(monitor):
    player = monitor.resolve_player()
    fields = monitor.profile['localPlayer']['position']
    result = {}
    for name in ('x', 'y', 'z'):
        offset = int(fields.get(name, '0x0'), 0)
        if not offset:
            raise ValueError('Posicao incompleta no perfil')
        result[name] = int(monitor.field(player, offset, 'uint32' if name in ('x', 'y') else 'byte'))
    if monitor.resolve_player() != player:
        raise RuntimeError('Jogador mudou durante a leitura')
    return result


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--profile', default='Antica_monitor.profile.json')
    parser.add_argument('--direction', choices=DIRECTIONS, default='north')
    parser.add_argument('--delay', type=int, default=5, help='Tempo para focar o jogo (3..30 segundos)')
    parser.add_argument('--pid', type=int)
    parser.add_argument('--input', choices=['sendinput', 'postmessage'], default='sendinput',
                        help='Metodo de entrada Windows (nao e chamada interna do OTC)')
    parser.add_argument('--dry-run', action='store_true', help='Somente ler posicao, sem enviar tecla')
    args = parser.parse_args()
    if sys.platform != 'win32' or struct.calcsize('P') != 8:
        parser.error('Use Windows e Python 64 bits')
    if not 3 <= args.delay <= 30:
        parser.error('delay deve estar entre 3 e 30 segundos')
    memory = None
    try:
        profile = json.loads(Path(args.profile).read_text(encoding='utf-8-sig', errors='replace'))
        memory = Memory(choose_pid('Antica Retro opengl.exe', args.pid))
        monitor = Monitor(memory, profile)
        before = position(monitor)
        print(f'Build e jogador conferidos. Posicao: {before}', flush=True)
        if args.dry_run:
            print('Simulacao: nenhuma tecla enviada.')
            return 0
        print(f'Uma seta: {args.direction}. Voce tem {args.delay}s para focar o jogo. '
              'Feche o chat, solte as teclas e fique em local livre.', flush=True)
        time.sleep(args.delay)
        before = position(monitor)
        keyboard = Keyboard(mode=args.input, target_pid=memory.pid)
        print(f'[ENTRADA] {args.input}', flush=True)
        press_once(keyboard, memory.pid, DIRECTIONS[args.direction][0])
        samples = []
        for _ in range(10):
            time.sleep(0.15)
            samples.append(position(monitor))
        after = samples[-1]
        print(f'Antes: {before}\nDepois: {after}')
        expected = {'x': before['x'] + DIRECTIONS[args.direction][1],
                    'y': before['y'] + DIRECTIONS[args.direction][2], 'z': before['z']}
        if after == expected:
            print('Um passo na direcao esperada foi observado. Nenhuma repeticao automatica.')
        elif after == before:
            print('Sem mudanca final de posicao. Pode haver obstaculo, chat ativo ou entrada nao aceita. Nao repetiu.')
        else:
            print('Posicao mudou de outra forma. Confira escada, movimento manual ou acoes anteriores. Nao repetiu.')
        return 0
    except (OSError, ValueError, RuntimeError, KeyError, TypeError, psutil.Error) as error:
        print(f'[ERRO] {error}', file=sys.stderr)
        return 1
    except KeyboardInterrupt:
        print('\nTeste encerrado.')
        return 130
    finally:
        if memory:
            memory.close()


if __name__ == '__main__':
    raise SystemExit(main())
