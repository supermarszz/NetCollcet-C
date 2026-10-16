"""Independent local Telnet fixture with deliberately fragmented negotiation."""
import socket
import threading
import time


class TelnetDeviceServer:
    protocol = "TELNET"

    def __init__(self, name, encoding="utf-8", mode="username", username="admin", password=" secret ", echo_password=False, close_on_command=False):
        self.name, self.encoding, self.mode = name, encoding, mode
        self.username, self.password = username, password
        self.echo_password, self.close_on_command = echo_password, close_on_command
        self.address = "127.0.0.1"
        self.commands, self.timestamps, self.errors = [], [], []
        self.negotiations, self.subnegotiations = [], []
        self.received_wire = bytearray()
        self.stop = threading.Event()
        self.sock = socket.socket()
        self.sock.bind((self.address, 0))
        self.sock.listen(8)
        self.sock.settimeout(0.2)
        self.port = self.sock.getsockname()[1]
        self.connections, self.threads = [], []
        self.thread = threading.Thread(target=self.accept, daemon=True)
        self.thread.start()

    @property
    def prompt(self):
        return f"<{self.name}>"

    def accept(self):
        while not self.stop.is_set():
            try:
                client, _ = self.sock.accept()
            except socket.timeout:
                continue
            except OSError:
                break
            client.settimeout(8)
            self.connections.append(client)
            thread = threading.Thread(target=self.serve, args=(client,), daemon=True)
            self.threads.append(thread)
            thread.start()

    def serve(self, client):
        state, command, sub = "data", 0, bytearray()
        application = bytearray()

        def read_data():
            nonlocal state, command, sub
            data = client.recv(4096)
            if not data:
                raise EOFError()
            self.received_wire.extend(data)
            for byte in data:
                if state == "data":
                    if byte == 255:
                        state = "iac"
                    else:
                        application.append(byte)
                elif state == "iac":
                    if byte == 255:
                        application.append(255)
                        state = "data"
                    elif byte in (251, 252, 253, 254):
                        command, state = byte, "option"
                    elif byte == 250:
                        sub, state = bytearray(), "sub"
                    else:
                        state = "data"
                elif state == "option":
                    self.negotiations.append((command, byte))
                    state = "data"
                elif state == "sub":
                    if byte == 255:
                        state = "sub_iac"
                    else:
                        sub.append(byte)
                elif state == "sub_iac":
                    if byte == 240:
                        self.subnegotiations.append((sub[0], bytes(sub[1:])))
                        state = "data"
                    elif byte == 255:
                        sub.append(255)
                        state = "sub"
                    else:
                        raise AssertionError("Malformed client Telnet subnegotiation")

        def line():
            while b"\n" not in application:
                read_data()
            end = application.index(10)
            value = bytes(application[:end]).rstrip(b"\r")
            del application[:end + 1]
            return value.decode(self.encoding)

        def data(value):
            encoded = value.encode(self.encoding) if isinstance(value, str) else value
            client.sendall(encoded.replace(b"\xff", b"\xff\xff"))

        def fragmented(value):
            encoded = value.encode(self.encoding)
            cut = max(1, len(encoded) // 2)
            data(encoded[:cut])
            time.sleep(0.02)
            data(encoded[cut:])

        try:
            # WILL ECHO/SGA/BINARY, DO TTYPE/NAWS/BINARY, and unsupported options.
            negotiation = b"\xff\xfb\x01\xff\xfb\x03\xff\xfd\x18\xff\xfd\x1f\xff\xfd\x00\xff\xfb\x00\xff\xfb\x63\xff\xfd\x64"
            client.sendall(negotiation[:1])
            time.sleep(0.02)
            client.sendall(negotiation[1:])
            # Request TTYPE with IAC/SB and SE split across socket writes.
            for chunk in (b"\xff\xfa\x18", b"\x01\xff", b"\xf0"):
                client.sendall(chunk)
                time.sleep(0.005)
            data("\x1b[32mWelcome\x1b[0m\r\n")
            if self.mode == "timeout":
                data("Waiting for authentication service...\r\n")
                self.stop.wait(3)
                return
            if self.mode != "direct":
                if self.mode != "password":
                    prompt = "用户名：" if self.mode == "chinese" else "login: " if self.mode == "login" else "Username: "
                    fragmented(prompt)
                    if line() != self.username:
                        data("\r\nLogin incorrect\r\n")
                        return
                fragmented("密码：" if self.mode == "chinese" else "Password: ")
                password = line()
                if self.echo_password:
                    data(password + "\r\n")
                if password != self.password or self.mode == "reject":
                    data("\r\nAuthentication failed\r\nUsername: ")
                    self.stop.wait(0.5)
                    return
                if self.mode in ("repeat", "change"):
                    data("\r\nNew password: " if self.mode == "change" else "\r\nPassword: ")
                    self.stop.wait(0.5)
                    return
            data("\r\n" + self.prompt)
            while not self.stop.is_set():
                value = line().strip()
                self.commands.append(value)
                self.timestamps.append(time.perf_counter())
                data(value + "\r\n")
                if self.close_on_command or value == "display disconnect":
                    data("partial before disconnect\r\n")
                    return
                if value == "display version":
                    output = "接口狀態正常\r\n" if self.encoding == "big5" else "normal port status\r\n" if self.encoding == "latin1" else "设备运行正常，接口状态正常\r\n"
                    encoded = output.encode(self.encoding)
                    data(encoded[:3])
                    client.sendall(b"\xff\xf1")  # NOP embedded between data fragments.
                    time.sleep(0.02)
                    data(encoded[3:])
                elif value == "display interface | include up":
                    data("GigabitEthernet0/0 up\r\n--Mo")
                    time.sleep(0.03)
                    data("re--")
                    while not application:
                        read_data()
                    if application.pop(0) != 32:
                        raise AssertionError("Pager did not receive space")
                    data("\b" * 8 + " " * 8 + "\b" * 8 + "GigabitEthernet0/1 up\r\n")
                elif value == "display big":
                    data("Port running normally\r\n" * 30000)
                elif value == "display slow":
                    data("first chunk\r\n")
                    self.stop.wait(1.6)
                    data("last chunk after quiet gap\r\n")
                elif value == "display stall":
                    data("partial output, no completion prompt\r\n")
                    self.stop.wait(2.5)
                    continue
                elif value == "show iac":
                    data(b"literal=\xff\r\n")
                else:
                    data("ok\r\n")
                data(self.prompt)
        except (EOFError, OSError):
            pass
        except Exception as exc:
            self.errors.append(repr(exc))
        finally:
            client.close()

    def close(self):
        self.stop.set()
        self.sock.close()
        for client in self.connections:
            try:
                client.shutdown(socket.SHUT_RDWR)
            except OSError:
                pass
            client.close()
        self.thread.join(2)
        for thread in self.threads:
            thread.join(4)
