from __future__ import annotations

import re

from PySide6.QtCore import QObject, QTimer, Signal


class SerialService(QObject):
    connected = Signal()
    disconnected = Signal()
    data_received = Signal(bytes)
    error_occurred = Signal(str)

    def __init__(self, parent=None):
        super().__init__(parent)
        self._serial = None
        self._read_timer = QTimer(self)
        self._read_timer.setInterval(30)
        self._read_timer.timeout.connect(self._read_available_data)

    def list_ports(self) -> tuple[str, ...]:
        try:
            from serial.tools import list_ports
        except ImportError as exc:
            raise RuntimeError("缺少 pyserial 依赖，无法扫描串口设备。") from exc

        ports = [port.device for port in list_ports.comports()]
        return tuple(sorted(ports, key=_port_sort_key))

    def open(
        self,
        port: str,
        baudrate: int,
        data_bits: str = "8",
        parity: str = "None",
        stop_bits: str = "1",
    ) -> None:
        try:
            import serial
        except ImportError as exc:
            raise RuntimeError("缺少 pyserial 依赖，无法打开串口设备。") from exc

        self.close()
        try:
            serial_obj = serial.Serial()
            serial_obj.port = port
            serial_obj.baudrate = baudrate
            serial_obj.bytesize = _serial_bytesize(serial, data_bits)
            serial_obj.parity = _serial_parity(serial, parity)
            serial_obj.stopbits = _serial_stopbits(serial, stop_bits)
            serial_obj.timeout = 0
            serial_obj.write_timeout = 0
            serial_obj.rtscts = False
            serial_obj.dsrdtr = False
            serial_obj.dtr = False
            serial_obj.rts = False
            serial_obj.open()
            serial_obj.dtr = False
            serial_obj.rts = False
            self._serial = serial_obj
        except Exception as exc:
            self._serial = None
            raise RuntimeError(f"串口打开失败：{exc}") from exc

        self._read_timer.start()
        self.connected.emit()

    def close(self) -> None:
        self._read_timer.stop()
        if self._serial is None:
            return
        serial_obj = self._serial
        self._serial = None
        try:
            if serial_obj.is_open:
                serial_obj.close()
        finally:
            self.disconnected.emit()

    def is_open(self) -> bool:
        return self._serial is not None and bool(self._serial.is_open)

    def write(self, data: bytes) -> None:
        if self._serial is None or not self._serial.is_open:
            raise RuntimeError("串口未连接。")
        try:
            self._serial.write(data)
        except Exception as exc:
            self.error_occurred.emit(f"串口发送失败：{exc}")
            raise RuntimeError(f"串口发送失败：{exc}") from exc

    def _read_available_data(self) -> None:
        if self._serial is None:
            return
        try:
            waiting = self._serial.in_waiting
            if waiting <= 0:
                return
            data = self._serial.read(waiting)
        except Exception as exc:
            self.error_occurred.emit(f"串口读取失败：{exc}")
            return
        if data:
            self.data_received.emit(data)


def _port_sort_key(port: str) -> tuple[str, int, str]:
    match = re.fullmatch(r"([A-Za-z]+)(\d+)", port)
    if not match:
        return (port.lower(), -1, port)
    prefix, number = match.groups()
    return (prefix.lower(), int(number), port)


def _serial_bytesize(serial_module, data_bits: str) -> int:
    return {
        "5": serial_module.FIVEBITS,
        "6": serial_module.SIXBITS,
        "7": serial_module.SEVENBITS,
        "8": serial_module.EIGHTBITS,
    }.get(data_bits, serial_module.EIGHTBITS)


def _serial_parity(serial_module, parity: str) -> str:
    return {
        "None": serial_module.PARITY_NONE,
        "Even": serial_module.PARITY_EVEN,
        "Odd": serial_module.PARITY_ODD,
        "Mark": serial_module.PARITY_MARK,
        "Space": serial_module.PARITY_SPACE,
    }.get(parity, serial_module.PARITY_NONE)


def _serial_stopbits(serial_module, stop_bits: str) -> float:
    return {
        "1": serial_module.STOPBITS_ONE,
        "1.5": serial_module.STOPBITS_ONE_POINT_FIVE,
        "2": serial_module.STOPBITS_TWO,
    }.get(stop_bits, serial_module.STOPBITS_ONE)
