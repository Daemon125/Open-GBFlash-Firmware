# -*- coding: utf-8 -*-
# FlashGBX
# Author: Lesserkuma (github.com/Lesserkuma)
# Modified for Open-GBFlash compatibility by Daemon125 (github.com/Daemon125)
# Firmware chooser in the updater from a design by Charlie SIGMA

# pylint: disable=wildcard-import, unused-wildcard-import
import configparser, datetime, os, struct, time, zipfile, zlib
from .app import AppInfo, AppContext
from .LK_Device import *
from .i18n import __, c__
from .IniSettings import IniSettings

OPEN_FW_ZIP = "fw_Open-GBFlash.zip"


def ReadOpenFirmwareZip(app_path):
	"""Describe res/fw_Open-GBFlash.zip, or None if it is not usable.

	Returning None has to leave the updater exactly as upstream's. An earlier
	design read both firmwares out of the vendor's own zip and refused to open at
	all when the second was missing, which cost the user the ability to install
	even the original.
	"""
	path = app_path + os.sep + os.path.join("res", OPEN_FW_ZIP)
	try:
		with zipfile.ZipFile(path) as z:
			z.read("fw.bin")
			ini = IniSettings(ini=z.open("fw.ini").read().decode("utf-8"),
			                  main_section="Firmware")
		ver = ini.GetValue("fw_ver")
		if not ver:
			return None
		try:
			ts = int(ini.GetValue("fw_buildts"))
		except (TypeError, ValueError):
			ts = None
		date = __("unknown date")
		if ts is not None:
			try:
				date = datetime.datetime.fromtimestamp(ts).strftime("%x")
			except (OverflowError, OSError, ValueError):
				ts = None
		return {"path": path, "ver": ver, "ts": ts, "date": date}
	except (OSError, zipfile.BadZipFile, KeyError, UnicodeDecodeError,
	        configparser.Error, zlib.error, EOFError):
		return None


class GbxDevice(LK_Device):
	DEVICE_NAME = "GBFlash"
	DEVICE_MIN_FW = 1
	DEVICE_MAX_FW = 12
	OPEN_FW = False
	DEVICE_LATEST_FW_TS = { 5:1780508702, 10:1780508702, 11:1780508702, 12:1780508702, 13:1780508702 }
	PCB_VERSIONS = { 5:'', 12:'v1.2', 13:'v1.3' }
	DEVICE_LABEL_LONG = "GBFlash"
	DEVICE_LABEL_SHORT = "GBFlash"
	FWUPDATE_ACTION = "fwupdate-gbflash"
	CLI_UPDATER_METHOD = "UpdateFirmwareGBFlash"
	DEVICE_SUPPORT_MESSAGE = "For help with your GBFlash, please visit the GitHub page:\nhttps://github.com/simonkwng/GBFlash"

	def __init__(self):
		pass

	def GetSupportMessage(self):
		# LK_Device reads DEVICE_SUPPORT_MESSAGE off type(self), so a value
		# assigned there for one device would outlive the connection that set it.
		if getattr(self, "OPEN_FW", False):
			return (
				"For help with the Open-GBFlash firmware, please visit the project page:\n"
				"https://github.com/Daemon125/Open-GBFlash-Firmware\n\n"
				"For the GBFlash hardware itself:\n"
				"https://github.com/simonkwng/GBFlash"
			)
		return LK_Device.GetSupportMessage(self)

	def _ResyncStalled(self, port):
		"""Recover a device left part way through a command.

		An interrupted transfer leaves the firmware's command parser waiting for
		argument bytes, so every later command is swallowed as arguments and the
		device answers nothing. It then looks absent rather than busy, and the
		only documented cure is unplugging it. Feeding it more zeros than any
		command consumes, then draining, puts the parser back on a command
		boundary.

		Restricted to 0x1209:0x0008, this firmware's own USB identity. The
		0x1A86:0x7523 branch of the port scan is a generic CH340, so it can be
		somebody's unrelated serial adapter, and writing 2 KB to that is not
		this tool's business.
		"""
		try:
			import serial.tools.list_ports
			if not any(p.device == port and (p.vid, p.pid) == (0x1209, 0x0008)
			           for p in serial.tools.list_ports.comports()):
				return False
			dev = serial.Serial(port, 2000000, timeout=0.5, exclusive=True)
		except Exception:
			return False
		try:
			for _ in range(3):
				dev.write(b"\x00" * 2048)
				dev.flush()
				time.sleep(0.3)
				while dev.read(65536):
					pass
				dev.reset_input_buffer()
				dev.write(b"\xA1")
				dev.flush()
				if dev.read(1)[:1] not in (b"", b"\x00"):
					dprint("Resynchronised a stalled device on", port)
					return True
			return False
		except Exception:
			return False
		finally:
			dev.close()

	def Initialize(self, flashcarts, port=None, max_baud=2000000):
		if self.IsConnected(): self.DEVICE.close()
		conn_msg = []
		ports = []
		if port is not None:
			ports = [ port ]
		else:
			comports = serial.tools.list_ports.comports()
			for i in range(0, len(comports)):
				# 0x1A86:0x7523 is the stock CH340. 0x1209:0x0008 is the open
				# firmware's own CDC-ACM identity (pid.codes); it speaks the
				# same protocol and reports the same cfw_id, so everything
				# below this point is unchanged. Must match BL_USB_CDC_PID in
				# work/firmware/include/usb.h.
				if (comports[i].vid == 0x1A86 and comports[i].pid == 0x7523) or \
				   (comports[i].vid == 0x1209 and comports[i].pid == 0x0008):
					ports.append(comports[i].device)
			if len(ports) == 0: return False

		for i in range(0, len(ports)):
			if not self.TryConnect(ports[i], max_baud) and \
			   not (self._ResyncStalled(ports[i]) and self.TryConnect(ports[i], max_baud)):
				continue
			self.BAUDRATE = max_baud
			dev = serial.Serial(ports[i], self.BAUDRATE, timeout=0.1)
			self.DEVICE = dev

			if self.FW is None or self.FW == {}: continue

			dprint(f"Found a {self.DEVICE_NAME}")
			dprint("Firmware information:", self.FW)
			# dprint("Baud rate:", self.BAUDRATE)

			# The device is asked what it is rather than assumed from its USB id,
			# because the identity is a build option and the firmware's name is not.
			self.OPEN_FW = (self.FW.get("pcb_name") or "").strip() == "Open-GBFlash"

			if self.DEVICE is None or not self.IsConnected():
				self.DEVICE = None
				if self.FW is not None:
					conn_msg.append([0, __("Couldn’t communicate with the {device_name} on port {port}. Please disconnect and reconnect the device, then try again.", device_name=self.DEVICE_NAME, port=ports[i])])
				continue
			elif self.FW is None or self.FW["pcb_ver"] not in self.PCB_VERSIONS.keys() or "cfw_id" not in self.FW or self.FW["cfw_id"] != 'L' or self.FW["fw_ver"] < self.DEVICE_MIN_FW: # Not a CFW by Lesserkuma
				dprint("Incompatible firmware:", self.FW)
				dev.close()
				self.DEVICE = None
				continue
			elif not self.OPEN_FW and self.FW["fw_ts"] > self.DEVICE_LATEST_FW_TS[self.FW["pcb_ver"]]:
				conn_msg.append([1, __("Note: The {device_name} on port {port} is running a firmware version that is newer than what this version of FlashGBX was developed to work with, so errors may occur.", device_name=self.DEVICE_NAME, port=ports[i])])

			# Everything below is applied only to Open-GBFlash. On the stock
			# firmware this file behaves exactly as the one it replaces, so a
			# user who switches back does not carry tuning meant for something
			# else.

			if not self.OPEN_FW:
				# Upstream's value, unchanged. Negotiating a larger buffer wins
				# stock nothing: it does not clamp, services whatever it is
				# handed, and reads no faster for it. On macOS it reads about
				# 22% SLOWER on Game Boy, which is a regression to hand someone
				# who has gone back to the vendor firmware.
				self.MAX_BUFFER_READ = 0x1000
			else:
				# Written and read back rather than assumed. Open-GBFlash clamps
				# to its own FW_MAX_TRANSFER, so the reply is its real ceiling,
				# and reading in larger pieces costs fewer round trips.
				negotiated = 0
				try:
					self._set_fw_variable("TRANSFER_SIZE", 0x8000)
					negotiated = self._get_fw_variable("TRANSFER_SIZE")
				except Exception:
					negotiated = 0
				if not isinstance(negotiated, int) or negotiated < 0x1000:
					negotiated = 0x1000
				self.MAX_BUFFER_READ = min(negotiated, 0x8000)
			self.MAX_BUFFER_WRITE = 0x800

			self.PORT = ports[i]
			self.DEVICE.timeout = self.DEVICE_TIMEOUT

			# Load Flash Cartridge Handlers
			self.UpdateFlashCarts(flashcarts)

			# Stop after first found device
			break

		return conn_msg

	def LoadFirmwareVersion(self):
		dprint("Querying firmware version")
		try:
			self.DEVICE.timeout = 0.075
			self.DEVICE.reset_input_buffer()
			self.DEVICE.reset_output_buffer()
			self._write(self.DEVICE_CMD["QUERY_FW_INFO"])
			size = self.DEVICE.read(1)
			self.DEVICE.timeout = self.DEVICE_TIMEOUT
			if len(size) == 0:
				dprint("No response")
				self.FW = None
				return False
			size = struct.unpack("B", size)[0]
			if size != 8: return False
			data = self._read(size)
			info = data[:8]
			keys = ["cfw_id", "fw_ver", "pcb_ver", "fw_ts"]
			values = struct.unpack(">cHBI", bytearray(info))
			self.FW = dict(zip(keys, values))
			self.FW["cfw_id"] = self.FW["cfw_id"].decode('ascii')
			self.FW["fw_dt"] = datetime.datetime.fromtimestamp(self.FW["fw_ts"]).astimezone().replace(microsecond=0).isoformat()
			self.FW["ofw_ver"] = None
			self.FW["pcb_name"] = None
			self.FW["cart_power_ctrl"] = False
			self.FW["bootloader_reset"] = False
			if self.FW["cfw_id"] == "L" and self.FW["fw_ver"] >= 12:
				size = self._read(1)
				name = self._read(size)
				if len(name) > 0:
					try:
						self.FW["pcb_name"] = name.decode("UTF-8").replace("\x00", "").strip()
					except:
						self.FW["pcb_name"] = "Unnamed Device"
					self.DEVICE_NAME = self.FW["pcb_name"]

				# Cartridge Power Control support, Switch Power support, and Switch Mode support
				temp = self._read(1)
				self.FW["cart_power_ctrl"] = True if temp & 1 == 1 else False
				self.FW["cart_presence_switch"] = True if (temp >> 1) & 1 == 1 else False
				self.FW["cart_mode_switch"] = True if (temp >> 2) & 1 == 1 else False

				# Reset to bootloader support
				temp = self._read(1)
				self.FW["bootloader_reset"] = True if temp & 1 == 1 else False
				self.FW["unregistered"] = True if temp >> 7 == 1 else False

			return True

		except Exception as e:
			dprint("Disconnecting due to an error", e, sep="\n")
			try:
				if self.DEVICE.isOpen():
					self.DEVICE.reset_input_buffer()
					self.DEVICE.reset_output_buffer()
					self.DEVICE.close()
				self.DEVICE = None
			except:
				pass
			return False

	def ChangeBaudRate(self, _):
		dprint("Baudrate change is not supported.")

	def GetFirmwareVersion(self, more=False):
		s = "{:s}{:d}".format(self.FW["cfw_id"], self.FW["fw_ver"])
		if self.FW["pcb_name"] is None:
			s = s + " <" + __("unverified") + ">"
		if more:
			return "{base} ({timestamp})".format(base=s, timestamp=self.FW["fw_dt"])
		return s

	def GetFullNameExtended(self, more=False):
		if more:
			return __("{device_name} – Firmware {fw_version} ({timestamp}) on {port}", device_name=self.GetFullName(), fw_version=self.GetFirmwareVersion(), timestamp=self.FW["fw_dt"], port=self.GetPort())
		else:
			return __("{device_name} – Firmware {fw_version} ({port})", device_name=self.GetFullName(), fw_version=self.GetFirmwareVersion(), port=self.GetPort())

	def CanSetVoltageBySwitch(self):
		return False

	def CanSetVoltageByCode(self):
		return True

	def CanSetVoltageByAutoswitch(self):
		return False

	def CanPowerCycleCart(self):
		return self.FW["cart_power_ctrl"]

	def GetSupprtedModes(self):
		return ["DMG", "AGB"]

	def IsSupported3dMemory(self):
		return True

	def IsClkConnected(self):
		return True

	def SupportsFirmwareUpdates(self):
		return True

	def FirmwareUpdateAvailable(self):
		if self.OPEN_FW:
			# DEVICE_LATEST_FW_TS holds stock build dates and says nothing about
			# this firmware. The image that would be installed is what to compare.
			if self._SkipOpenFirmwareUpdate():
				return False
			ofw = ReadOpenFirmwareZip(AppContext.APP_PATH)
			if ofw is None or ofw["ts"] is None:
				return False
			return ofw["ts"] > self.FW["fw_ts"]
		if self.FW["pcb_ver"] == 5 or self.FW["fw_ts"] < 1730592000: # unofficial firmware
			self.FW_UPDATE_REQ = True
			return True
		if self.FW["fw_ts"] != self.DEVICE_LATEST_FW_TS[self.FW["pcb_ver"]]:
			return True
		self.FW_UPDATE_REQ = False
		return False

	def _SkipOpenFirmwareUpdate(self):
		# FlashGBX builds an "Ignore firmware updates" checkbox for this prompt and
		# never passes it to setCheckBox(), so the prompt has no off switch of its
		# own. SkipFirmwareUpdate is global and silences the other devices too.
		try:
			ini = IniSettings(path=AppContext.CONFIG_PATH + os.sep + "settings.ini")
			return str(ini.GetValue("SkipOpenFirmwareUpdate")).lower() == "enabled"
		except Exception:
			return False

	def GetFirmwareUpdaterClass(self):
		try:
			return (None, FirmwareUpdaterWindow)
		except:
			return None

	def ResetLEDs(self):
		pass

	def SupportsBootloaderReset(self):
		return self.FW["bootloader_reset"]

	def BootloaderReset(self):
		if not self.SupportsBootloaderReset(): return False
		dprint("Resetting to bootloader...")
		try:
			self._write(self.DEVICE_CMD["BOOTLOADER_RESET"], wait=True)
			self._write(1)
			# BOOTLOADER_RESET is two bytes: the first draws an ack, the second
			# confirms and the device jumps. Closing straight after writing that
			# confirm can discard it.
			#
			# flush() drains to the driver, not across the USB pipe. On POSIX it
			# is tcdrain and blocks until the bytes are gone, which is why this
			# has always worked on macOS. On Windows it polls out_waiting, and
			# the byte can still be in flight when the handle closes, so the
			# device stays on its application firmware and the user is told to
			# hold U22. Wait for the queue to empty, then give it a moment on
			# the wire.
			try:
				self.DEVICE.flush()
				end = time.time() + 2.0
				while self.DEVICE.out_waiting and time.time() < end:
					time.sleep(0.02)
			except (OSError, AttributeError, NotImplementedError):
				pass
			time.sleep(0.4)
			self.DEVICE.close()
			return True
		except Exception as e:
			print(__("Disconnecting..."), e)
			return False

	def SupportsAudioAsWe(self):
		return not (self.FW["pcb_ver"] < 13 and self.CanPowerCycleCart())

	def GetMode(self):
		if self.FW["fw_ts"] == 1681900614: return self.MODE
		return super().GetMode()

	def SetAutoPowerOff(self, value):
		value &= 0xFFFFFFFF
		return super().SetAutoPowerOff(value)

	def GetFullName(self):
		if self.FW["pcb_ver"] < 13 and self.CanPowerCycleCart():
			s = "{device_name} {pcb_version} + PLUGIN 01".format(device_name=self.GetName(), pcb_version=self.GetPCBVersion())
		else:
			s = "{:s} {:s}".format(self.GetName(), self.GetPCBVersion())
		if self.IsUnregistered():
			s += " (" + __("unregistered") + ")"
		return s

	def ReadROM(self, address, length, skip_init=False, max_length=64):
		"""Upstream's ReadROM with one read opcode kept outstanding.

		Most of this body is LK_Device.ReadROM's; the loop had to be owned to
		pipeline it. host/test_upstream_drift.py fails if upstream's changes.

		The read opcodes carry no argument bytes and the device services its RX
		ring while a reply is still streaming, so the opcode for the next region
		is already in hand when the current one ends. Depth 2 is the whole gain;
		deeper queues measure the same and only widen the recovery window.
		"""
		if not getattr(self, "OPEN_FW", False):
			return LK_Device.ReadROM(self, address, length, skip_init, max_length)

		max_length = min(max_length, self.MAX_BUFFER_READ)
		num = -(-length // max_length)
		dprint("Reading 0x{:X} bytes from cartridge ROM at 0x{:X} in {:d} iteration(s)".format(length, address, num))
		if length > max_length: length = max_length

		buffer = bytearray()
		if not skip_init:
			# Only ADDRESS changes between calls. _BackupROM reads DMG one bank
			# per call (LK_Device.py:3003), so the other two cost a blocking
			# round trip every 16 KiB for a value the device already holds.
			# _set_fw_variable below drops the memo, so anything else that
			# writes these makes the next call send them again; a stale memo
			# costs a short read, which _BackupROM already retries.
			memo = getattr(self, "_rom_var_memo", None)
			if memo is None or memo[0] is not self.DEVICE:
				memo = [self.DEVICE, None, None]
			send_xfer = (memo[1] != length)
			send_mode = (memo[2] != 1)
			if send_xfer:
				self._set_fw_variable("TRANSFER_SIZE", length)
			if self.MODE == "DMG":
				self._set_fw_variable("ADDRESS", address)
				if send_mode:
					self._set_fw_variable("DMG_ACCESS_MODE", 1) # MODE_ROM_READ
			elif self.MODE == "AGB":
				self._set_fw_variable("ADDRESS", address >> 1)
			self._rom_var_memo = [self.DEVICE, length,
								  1 if self.MODE == "DMG" else memo[2]]

		if self.MODE == "DMG":
			command = "DMG_CART_READ"
		elif self.MODE == "AGB":
			command = "AGB_CART_READ"
		else:
			raise NotImplementedError

		cmd = self.DEVICE_CMD[command]
		issued = 0
		if num > 0:
			self._write(cmd)
			issued = 1

		for n in range(0, num):
			if issued < num:
				self._write(cmd)
				issued += 1
			temp = self._read(length)
			if temp is not False and isinstance(temp, int): temp = bytearray([temp])
			if temp is False or len(temp) != length:
				dprint("Error while trying to read 0x{:X} bytes from cartridge ROM at 0x{:X} in iteration {:d} of {:d} (response: {:s})".format(length, address, n, num, str(temp)))
				# An opcode is still outstanding here, so a reply is still owed.
				self.DEVICE.reset_output_buffer()
				self._drain_outstanding()
				return bytearray()
			buffer += temp
			if self.INFO["action"] in (self.ACTIONS["ROM_READ"], self.ACTIONS["SAVE_READ"], self.ACTIONS["ROM_WRITE_VERIFY"]) and not self.NO_PROG_UPDATE:
				self.SetProgress({"action":"READ", "bytes_added":len(temp)})

		# Every reply asked for has been read, so the port must be empty. A
		# spare byte here is read as the caller's next ACK and shifts the rest
		# of the dump by one, which only a ROM checksum would catch.
		if self.DEVICE.in_waiting != 0:
			dprint("ReadROM: {:d} unexpected byte(s) left after 0x{:X} at 0x{:X}".format(self.DEVICE.in_waiting, length * num, address))
			self._drain_outstanding()
			return bytearray()

		return buffer

	def SetAGBReadMethod(self, method):
		"""Keep Stream through a ROM dump of an enable_pullups profile.

		LK_Device.py:2925-2927 enables the pullups and drops to Single for the
		whole dump, restoring only at :3232, so three returns in between leak
		Single into the rest of the session. Single re-latches /CS every two
		bytes against Stream's cart_latch of 128.

		Only the automatic downgrade: INFO["action"] is ROM_READ only inside
		_BackupROM_Worker, so a method the user picks from the menu still
		applies. GATED ON ONE CARTRIDGE, the AGB-E20-30 with S29GL256N10TFI01;
		the other three names in fc_AGB_S29GL256.txt and all of fc_AGB_M29W640
		are untested. See results/agb-read-method-downgrade.md.
		"""
		if (getattr(self, "OPEN_FW", False) and method == 0
				and self.AGB_READ_METHOD == 2
				and self.INFO.get("action") == self.ACTIONS["ROM_READ"]):
			return
		return LK_Device.SetAGBReadMethod(self, method)

	def _set_fw_variable(self, key, value):
		"""Forget what ReadROM remembers whenever anyone else sets these.

		Unguarded by OPEN_FW on purpose: it only clears a memo that the guarded
		ReadROM reads, and always delegates, so stock firmware sees upstream.
		"""
		if key in ("TRANSFER_SIZE", "DMG_ACCESS_MODE"):
			self._rom_var_memo = None
		return LK_Device._set_fw_variable(self, key, value)

	def _try_write(self, data, retries=5):
		"""Upstream's _try_write, with the port quieted after the resync.

		Most of this body is LK_Device._try_write's; the fix sits inside its
		loop. host/test_upstream_drift.py fails if upstream's changes.

		The resync writes 0x00 and takes the next byte as its answer. An ACK
		that lands between the reset_input_buffer() above it and that read is
		taken instead; the 0x00's own ACK is then read as the re-sent command's,
		and the command's real ACK is left to be read as the first byte of the
		next bulk transfer, which shifts the rest of a ROM dump by one. Measured
		once in 15 sixteen-megabyte dumps, on the stock read path as well as the
		pipelined one. host/test_late_ack.py holds the case.
		"""
		if not getattr(self, "OPEN_FW", False):
			return LK_Device._try_write(self, data, retries)

		while retries > 0:
			ack = self._write(data, wait=True)
			if "from_user" in self.CANCEL_ARGS and self.CANCEL_ARGS["from_user"]:
				return False
			if ack is not False:
				self.ERROR = False
				self.CANCEL = False
				self.CANCEL_ARGS = {}
				return ack
			retries -= 1
			dprint("Retries left:", retries)

			hp = 20
			temp = 0
			while temp not in (1, 2) and hp > 0:
				self.DEVICE.reset_output_buffer()
				self.DEVICE.reset_input_buffer()
				self.DEVICE.write(b'\x00')
				self.DEVICE.flush()
				temp = self._read(1)
				hp -= 1
				dprint("Current response:", temp, ", HP:", hp)
			self._drain_outstanding()
		return False

	def _drain_outstanding(self):
		"""Discard whatever the device is still sending.

		The deadline caps the stall per failed chunk; _BackupROM retries 20
		times before giving up.
		"""
		quiet = 0
		deadline = time.time() + 2.0
		while quiet < 20 and time.time() < deadline:
			if self.DEVICE.in_waiting > 0:
				self.DEVICE.read(self.DEVICE.in_waiting)
				quiet = 0
			else:
				quiet += 1
				time.sleep(0.01)
		self.DEVICE.reset_input_buffer()

	def GetRegisterInformation(self):
		text = __("Your GBFlash device reported a registration error, which means it may be an illegitimate clone.") + "<br><br>" + __("The device’s integrated piracy detection may limit the device in performance and functionality until proper registration. The FlashGBX software has no control over this.")
		return text


class FirmwareUpdater():
	PORT = None
	DEVICE = None

	def __init__(self, app_path=".", port=None):
		self.APP_PATH = app_path
		self.PORT = port

	def PackPacket(self, packet):
		values = list(packet.values())[:-2]
		data = struct.pack(">IBHHH", *values)
		if packet["payload_len"] > 0:
			data += list(packet.values())[-2]
		data += struct.pack(">I", packet["outro"])
		if len(data) % 2 == 1: data += b'\00'
		return data

	def GetPacket(self):
		hp = 100
		while self.DEVICE.in_waiting == 0:
			time.sleep(0.001)
			hp -= 1
			if hp <= 0:
				return None
		keys = ["intro", "sender", "seq_no", "command", "payload_len"]
		try:
			temp = self.DEVICE.read(11)
			if temp is False: temp = b''
			values = struct.unpack(">IBHHH", temp)
		except (struct.error, TypeError):
			return {"clone":True, "error": "Bootloader error! " + ''.join(format(x, '02X') for x in temp)}
		try:
			data = dict(zip(keys, values))
			data["payload"] = self.DEVICE.read(data["payload_len"])
			data["outro"] = self.DEVICE.read(4)
			if data["outro"] is False: data["outro"] = b''
			data["outro"] = struct.unpack(">I", data["outro"])[0]
		except struct.error:
			return {"clone":True, "error": "Erroneous outro response! " + ''.join(format(x, '02X') for x in data["outro"])}
		return data

	def CRC16(self, data):
		CRCTableAbs = [
			0x0000, 0xCC01, 0xD801, 0x1400, 0xF001, 0x3C00, 0x2800, 0xE401,
			0xA001, 0x6C00, 0x7800, 0xB401, 0x5000, 0x9C01, 0x8801, 0x4400,
		]
		wCRC = 0xFFFF

		for i in range(len(data)):
			chChar = data[i]
			wCRC = (CRCTableAbs[(chChar ^ wCRC) & 0x0F] ^ (wCRC >> 4))
			wCRC = (CRCTableAbs[((chChar >> 4) ^ wCRC) & 0x0F] ^ (wCRC >> 4))

		return wCRC

	def TryConnect(self, port):
		seq_no = 1
		packet = {
			"intro":0x48484A4A,
			"sender":0,
			"seq_no":seq_no,
			"command":0x21,
			"payload_len":0,
			"payload":bytearray(),
			"outro":0x4A4A4848,
		}
		data = self.PackPacket(packet)

		self.DEVICE = None
		try:
			# The bootloader is always a CH340 at 1A86:7523. A firmware with its
			# own USB identity comes back under a different port name across the
			# handover, and the caller may already have reset into the bootloader,
			# so the recorded path can be gone before this runs. Resolve both
			# before opening anything.
			bl_port = None
			have_port = False
			for p in serial.tools.list_ports.comports():
				if p.vid == 0x1A86 and p.pid == 0x7523 and bl_port is None:
					bl_port = p.device
				if p.device == port:
					have_port = True

			if bl_port is not None and not have_port:
				# Already in the bootloader under a new name. Nothing to hand over.
				self.DEVICE = serial.Serial(bl_port, 2000000, timeout=0.5)
			else:
				self.DEVICE = serial.Serial(port, 2000000, timeout=0.5)
				self.DEVICE.write(b'\xF1')
				self.DEVICE.read(1)
				# 0xF1 draws an ack, 0x01 confirms and the device jumps. Wait for
				# the write queue to drain before closing; flush() only reaches the
				# driver, not across the USB pipe.
				self.DEVICE.write(b'\x01')
				self.DEVICE.flush()
				try:
					_end = time.time() + 2.0
					while self.DEVICE.out_waiting and time.time() < _end:
						time.sleep(0.02)
				except (OSError, AttributeError, NotImplementedError):
					pass
				time.sleep(0.4)
				self.DEVICE.close()
				time.sleep(3)
				bl_port = port
				for p in serial.tools.list_ports.comports():
					if p.vid == 0x1A86 and p.pid == 0x7523:
						bl_port = p.device
						break
				self.DEVICE = serial.Serial(bl_port, 2000000, timeout=0.5)

		except serial.serialutil.SerialException:
			return False

		self.DEVICE.write(data)
		time.sleep(0.1)
		self.DEVICE.read(self.DEVICE.in_waiting)
		self.DEVICE.write(data)
		data = self.GetPacket()
		if data is None:
			self.DEVICE = None
			return False
		if "error" in data:
			self.DEVICE = None
			return data
		if data["seq_no"] != seq_no:
			self.DEVICE = None
			return False
		if data["command"] != 0x21:
			self.DEVICE = None
			return False
		if struct.unpack(">H", data["payload"][1:3])[0] != 0x03:
			self.DEVICE = None
			return False
		return data

	def WriteFirmware(self, zipfn, fncSetStatus):
		try:
			with zipfile.ZipFile(zipfn) as archive:
				with archive.open("fw.bin") as f: fw_data = bytearray(f.read())
		except (zipfile.BadZipFile, KeyError, zlib.error, EOFError):
			fncSetStatus(__("The firmware update file is corrupted."))
			return 2

		fncSetStatus(__("Connecting..."))
		data = False
		if self.PORT is None:
			ports = []
			comports = serial.tools.list_ports.comports()
			for i in range(0, len(comports)):
				# 0x1A86:0x7523 is the stock CH340 identity and the bootloader.
				# 0x1209:0x0008 is the open firmware's own CDC identity.
				if (comports[i].vid == 0x1A86 and comports[i].pid == 0x7523) or \
				   (comports[i].vid == 0x1209 and comports[i].pid == 0x0008):
					ports.append(comports[i].device)
			if len(ports) == 0:
				fncSetStatus(__("No device found."))
				return 2

			for port in ports:
				data = self.TryConnect(port)
				if data is not False:
					break
		else:
			data = self.TryConnect(self.PORT)

		if isinstance(data, dict) and "error" in data:
			fncSetStatus(text=data["error"], cloneError="clone" in data and data["clone"] is True)
			return 2
		if not isinstance(data, dict) or self.DEVICE is None:
			fncSetStatus(__("No device found."))
			return 2

		data["program_size"] = struct.unpack(">H", data["payload"][3:5])[0]
		data["page_size"] = struct.unpack(">H", data["payload"][7:9])[0]
		page_size = data["page_size"]
		num_packets = len(fw_data) / page_size
		num_packets = int(-(-num_packets // 1)) # round up

		fncSetStatus(__("Updating firmware..."), setProgress=0)
		seq_no = 2

		pos = 0
		packet_index = 1
		while pos < num_packets:
			buffer = fw_data[pos*page_size:pos*page_size+page_size]
			packet_len = len(buffer)
			buffer += struct.pack(">H", self.CRC16(buffer))
			buffer = struct.pack(">H", packet_len) + buffer
			buffer = struct.pack(">H", packet_index) + buffer

			packet = {
				"intro":0x48484A4A,
				"sender":0,
				"seq_no":seq_no,
				"command":0x24,
				"payload_len":len(buffer),
				"payload":buffer,
				"outro":0x4A4A4848,
			}
			data = self.PackPacket(packet)

			self.DEVICE.write(data)
			data = self.GetPacket()
			if data is None:
				fncSetStatus(__("No response from device."))
				return 2
			if data["seq_no"] != seq_no:
				fncSetStatus(__("Incorrect sequence number."))
				time.sleep(1)
				continue
			if data["command"] != 0x24:
				fncSetStatus(__("Incorrect command."))
				time.sleep(1)
				continue
			if struct.unpack(">H", data["payload"][0:2])[0] != packet_index:
				fncSetStatus(__("Incorrect data packet number."))
				time.sleep(1)
				continue
			if data["payload"][2] != 0x01:
				fncSetStatus(__("Write failed."))
				time.sleep(1)
				continue

			percent = packet_index / num_packets * 100
			fncSetStatus(text=__("Updating firmware... Do not unplug the device!"), setProgress=percent)
			pos += 1
			seq_no += 1
			packet_index += 1

		if pos == num_packets:
			payload = bytearray()
			payload += struct.pack(">H", self.CRC16(fw_data))
			payload += struct.pack(">H", ~self.CRC16(fw_data) & 0xFFFF)
			packet = {
				"intro":0x48484A4A,
				"sender":0,
				"seq_no":seq_no,
				"command":0x23,
				"payload_len":4,
				"payload":payload,
				"outro":0x4A4A4848,
			}
			data = self.PackPacket(packet)
			self.DEVICE.write(data)
			data = self.GetPacket()
			if data is None:
				fncSetStatus(__("No response from device."))
				return 2
			if data["payload"][0] != 1:
				fncSetStatus(text=__("Update failed!"), enableUI=True)

			self.DEVICE.close()
			time.sleep(0.8)
			fncSetStatus(__("Done!"))
			time.sleep(0.2)
			return 1


try:
	from .pyside import QtCore, QtWidgets, QtGui, QDesktopWidget

	class FirmwareUpdaterWindow(QtWidgets.QDialog):
		APP = None
		DEVICE = None
		FWUPD = None
		DEV_NAME = "GBFlash"
		FW_VER = ""
		PCB_VER = ""

		def __init__(self, app, app_path, file=None, icon=None, device=None):
			QtWidgets.QDialog.__init__(self, app)
			if icon is not None: self.setWindowIcon(QtGui.QIcon(icon))
			self.setStyleSheet("QMessageBox { messagebox-text-interaction-flags: 5; }")
			self.setWindowTitle(AppInfo.NAME + " – " + __("Firmware Updater for {device_name}", device_name="GBFlash"))
			self.setWindowFlags((self.windowFlags() | QtCore.Qt.MSWindowsFixedSizeDialogHint) & ~QtCore.Qt.WindowContextHelpButtonHint)

			self.APP = app
			if device is not None:
				self.FWUPD = FirmwareUpdater(app_path, device.GetPort())
				self.DEV_NAME = device.GetName()
				self.FW_VER = device.GetFirmwareVersion(more=True)
				self.PCB_VER = device.GetPCBVersion()
				self.DEVICE = device
			else:
				self.APP.QT_APP.processEvents()
				self.FWUPD = FirmwareUpdater(app_path, None)

			self.layout = QtWidgets.QGridLayout()
			self.layout.setContentsMargins(-1, 8, -1, 8)
			self.layout.setSizeConstraint(QtWidgets.QLayout.SetFixedSize)
			self.layout_device = QtWidgets.QVBoxLayout()

			# ↓↓↓ Current Device Information
			self.grpDeviceInfo = QtWidgets.QGroupBox(__("Current Firmware"))
			self.grpDeviceInfo.setMinimumWidth(420)
			self.grpDeviceInfoLayout = QtWidgets.QVBoxLayout()
			self.grpDeviceInfoLayout.setContentsMargins(-1, 3, -1, -1)
			rowDeviceInfo1 = QtWidgets.QHBoxLayout()
			self.lblDeviceName = QtWidgets.QLabel(__("Device:"))
			self.lblDeviceName.setMinimumWidth(120)
			self.lblDeviceNameResult = QtWidgets.QLabel("GBFlash")
			rowDeviceInfo1.addWidget(self.lblDeviceName)
			rowDeviceInfo1.addWidget(self.lblDeviceNameResult)
			rowDeviceInfo1.addStretch(1)
			self.grpDeviceInfoLayout.addLayout(rowDeviceInfo1)
			rowDeviceInfo3 = QtWidgets.QHBoxLayout()
			self.lblDeviceFWVer = QtWidgets.QLabel(__("Firmware version:"))
			self.lblDeviceFWVer.setMinimumWidth(120)
			self.lblDeviceFWVerResult = QtWidgets.QLabel("")
			rowDeviceInfo3.addWidget(self.lblDeviceFWVer)
			rowDeviceInfo3.addWidget(self.lblDeviceFWVerResult)
			rowDeviceInfo3.addStretch(1)
			self.grpDeviceInfoLayout.addLayout(rowDeviceInfo3)
			self.grpDeviceInfo.setLayout(self.grpDeviceInfoLayout)
			self.layout_device.addWidget(self.grpDeviceInfo)
			# ↑↑↑ Current Device Information

			# ↓↓↓ Available Firmware Updates
			self.grpAvailableFwUpdates = QtWidgets.QGroupBox(__("Available Firmware"))
			self.grpAvailableFwUpdates.setMinimumWidth(400)
			self.grpAvailableFwUpdatesLayout = QtWidgets.QVBoxLayout()
			self.grpAvailableFwUpdatesLayout.setContentsMargins(-1, 3, -1, -1)

			rowDeviceInfo4 = QtWidgets.QHBoxLayout()
			self.lblDeviceFWVer2 = QtWidgets.QLabel(__("Firmware version:"))
			self.lblDeviceFWVer2.setMinimumWidth(120)
			self.lblDeviceFWVer2Result = QtWidgets.QLabel("(" + __("Please choose the PCB version") + ")")
			rowDeviceInfo4.addWidget(self.lblDeviceFWVer2)
			rowDeviceInfo4.addWidget(self.lblDeviceFWVer2Result)
			rowDeviceInfo4.addStretch(1)

			# A second firmware, in its own zip beside the vendor's rather than
			# merged into it. The vendor file is never touched, so a FlashGBX
			# upgrade replaces theirs and leaves this one alone, and going back
			# to the original is a radio button rather than restoring a backup.
			# Absent, everything below is skipped and this dialog is upstream's.
			self.optFWOriginal = None
			self.optFWOpen = None
			ofw = self.OpenFirmwareInfo()
			if ofw is not None:
				self.optFWOriginal = QtWidgets.QRadioButton(__("Original firmware"))
				self.optFWOriginal.setChecked(True)
				lblOrig = QtWidgets.QLabel(
					"<ul><li>" + __("Maintained by Lesserkuma, shipped with FlashGBX") + "</li></ul>")
				lblOrig.setWordWrap(True)
				lblOrig.mousePressEvent = lambda x: self.optFWOriginal.setChecked(True)

				self.optFWOpen = QtWidgets.QRadioButton(
					__("Open-GBFlash {version} ({date})", version=ofw["ver"], date=ofw["date"]))
				lblOpen = QtWidgets.QLabel(
					"<ul><li>" + __("Third-party firmware by Daemon125") + "</li><li>"
					+ __("Not supported by Lesserkuma; report problems to the project:")
					+ "<br>https://github.com/Daemon125/Open-GBFlash-Firmware"
					+ "</li><li>" + __("Use at your own risk") + "</li></ul>")
				lblOpen.setWordWrap(True)
				lblOpen.mousePressEvent = lambda x: self.optFWOpen.setChecked(True)

				dev_ts = (getattr(self.DEVICE, "FW", None) or {}).get("fw_ts")
				if ofw["ts"] is not None and dev_ts is not None and ofw["ts"] > dev_ts:
					self.optFWOpen.setChecked(True)

				self.grpAvailableFwUpdatesLayout.addWidget(self.optFWOriginal)
				self.grpAvailableFwUpdatesLayout.addWidget(lblOrig)
				self.grpAvailableFwUpdatesLayout.addWidget(self.optFWOpen)
				self.grpAvailableFwUpdatesLayout.addWidget(lblOpen)
			else:
				self.grpAvailableFwUpdatesLayout.addLayout(rowDeviceInfo4)

			self.rowUpdate = QtWidgets.QHBoxLayout()
			self.btnUpdate = QtWidgets.QPushButton(__("Install Firmware Update"))
			self.btnUpdate.setMinimumWidth(200)
			self.btnUpdate.setContentsMargins(20, 20, 20, 20)
			self.connect(self.btnUpdate, QtCore.SIGNAL("clicked()"), lambda: [ self.UpdateFirmware() ])
			self.rowUpdate.addStretch()
			self.rowUpdate.addWidget(self.btnUpdate)
			self.rowUpdate.addStretch()

			self.grpAvailableFwUpdatesLayout.addSpacing(3)
			self.grpAvailableFwUpdatesLayout.addItem(self.rowUpdate)
			self.grpAvailableFwUpdates.setLayout(self.grpAvailableFwUpdatesLayout)
			self.layout_device.addWidget(self.grpAvailableFwUpdates)
			# ↑↑↑ Available Firmware Updates

			self.grpStatus = QtWidgets.QGroupBox("")
			self.grpStatusLayout = QtWidgets.QGridLayout()
			self.prgStatus = QtWidgets.QProgressBar()
			self.prgStatus.setMinimum(0)
			self.prgStatus.setMaximum(1000)
			self.prgStatus.setValue(0)
			self.lblStatus = QtWidgets.QLabel(__("Status: Ready."))

			self.grpStatusLayout.addWidget(self.prgStatus, 1, 0)
			self.grpStatusLayout.addWidget(self.lblStatus, 2, 0)

			self.grpStatus.setLayout(self.grpStatusLayout)
			self.layout_device.addWidget(self.grpStatus)

			self.grpFooterLayout = QtWidgets.QHBoxLayout()
			self.btnClose = QtWidgets.QPushButton(c__("Button (& = Keyboard Shortcut)", "&Close"))
			self.connect(self.btnClose, QtCore.SIGNAL("clicked()"), lambda: [ self.reject() ])
			self.grpFooterLayout.addStretch()
			self.grpFooterLayout.addWidget(self.btnClose)
			self.layout_device.addItem(self.grpFooterLayout)

			self.layout.addLayout(self.layout_device, 0, 0)
			self.setLayout(self.layout)

			self.lblDeviceNameResult.setText(self.DEV_NAME + " " + self.PCB_VER)
			self.lblDeviceFWVerResult.setText(self.FW_VER)
			self.SetPCBVersion()

		def OpenFirmwareInfo(self):
			return ReadOpenFirmwareZip(self.FWUPD.APP_PATH)

		def SetPCBVersion(self):
			file_name = self.FWUPD.APP_PATH + os.sep + os.path.join("res", "fw_GBFlash.zip")

			with zipfile.ZipFile(file_name) as zip:
				with zip.open("fw.ini") as f: ini_file = f.read()
				ini_file = ini_file.decode(encoding="utf-8")
				self.INI = IniSettings(ini=ini_file, main_section="Firmware")
				self.OFW_VER = self.INI.GetValue("fw_ver")
				self.OFW_BUILDTS = self.INI.GetValue("fw_buildts")
				self.OFW_TEXT = self.INI.GetValue("fw_text")

			if self.optFWOriginal is None:
				self.lblDeviceFWVer2Result.setText("{:s} ({:s})".format(self.OFW_VER, datetime.datetime.fromtimestamp(int(self.OFW_BUILDTS)).astimezone().replace(microsecond=0).isoformat()))
			else:
				self.optFWOriginal.setText(__("Original firmware {version} ({date})", version=self.OFW_VER, date=datetime.datetime.fromtimestamp(int(self.OFW_BUILDTS)).strftime("%x")))

		def run(self):
			try:
				self.layout.update()
				self.layout.activate()
				screenGeometry = QDesktopWidget().screenGeometry(self)
				x = (screenGeometry.width() - self.width()) / 2
				y = (screenGeometry.height() - self.height()) / 2
				self.move(x, y)
				self.show()
			except:
				return

		def hideEvent(self, event):
			if self.DEVICE is None:
				self.APP.ConnectDevice()
			self.APP.activateWindow()

		def reject(self):
			if self.CloseDialog():
				super().reject()

		def CloseDialog(self):
			if self.btnClose.isEnabled() is False:
				text = __("<b>Warning:</b> If you close this window while a firmware update is still running, it might leave the device in an unbootable state.") + " " + __("You can still recover it by running the Firmware Updater again later.") + "<br><br>" + __("Are you sure you want to close this window?")
				msgbox = QtWidgets.QMessageBox(parent=self, icon=QtWidgets.QMessageBox.Warning, windowTitle=AppInfo.NAME, text=text, standardButtons=QtWidgets.QMessageBox.Yes | QtWidgets.QMessageBox.No)
				msgbox.setDefaultButton(QtWidgets.QMessageBox.No)
				answer = msgbox.exec()
				if answer == QtWidgets.QMessageBox.No: return False
			return True

		def UpdateFirmware(self):
			file_name = self.FWUPD.APP_PATH + os.sep + os.path.join("res", "fw_GBFlash.zip")

			# Each zip carries its own fw.bin, so choosing a firmware is choosing
			# a file. WriteFirmware is untouched.
			if self.optFWOpen is not None and self.optFWOpen.isChecked():
				ofw = self.OpenFirmwareInfo()
				if ofw is None:
					QtWidgets.QMessageBox.critical(self, AppInfo.NAME,
						__("{file} could not be read.", file=OPEN_FW_ZIP))
					return False
				file_name = ofw["path"]
				text = __("This will install Open-GBFlash {version}, which is not the firmware that came with your device and is not maintained by the author of FlashGBX.", version=ofw["ver"])
				text += "\n\n" + __("Project page:") + "\nhttps://github.com/Daemon125/Open-GBFlash-Firmware"
				text += "\n\n" + __("You can return to the original firmware at any time from this window.")
				text += "\n\n" + __("Do you want to continue?")
				msgbox = QtWidgets.QMessageBox(parent=self, icon=QtWidgets.QMessageBox.Question, windowTitle=AppInfo.NAME, text=text, standardButtons=QtWidgets.QMessageBox.Yes | QtWidgets.QMessageBox.No)
				msgbox.setDefaultButton(QtWidgets.QMessageBox.No)
				if msgbox.exec() == QtWidgets.QMessageBox.No: return False

			elif self.optFWOpen is not None and getattr(self.APP.CONN, "OPEN_FW", False):
				# The handover changes the USB id and the port name with it.
				# FirmwareUpdater.TryConnect looks the bootloader up by VID/PID.
				text = __("This will replace Open-GBFlash with the original firmware.")
				text += "\n\n" + __("The device is put into update mode over the connection already open, so the U22 button should not be needed. If the update fails, unplug the device, hold the small button (U22) while plugging the USB cable back in, then run the updater again.")
				text += "\n\n" + __("Click OK to continue.")
				msgbox = QtWidgets.QMessageBox(parent=self, icon=QtWidgets.QMessageBox.Information, windowTitle=AppInfo.NAME, text=text, standardButtons=QtWidgets.QMessageBox.Ok | QtWidgets.QMessageBox.Cancel)
				msgbox.setDefaultButton(QtWidgets.QMessageBox.Ok)
				if msgbox.exec() == QtWidgets.QMessageBox.Cancel: return False

			if self.APP.CONN is None or self.APP.CONN.BootloaderReset() is False:
				self.APP.DisconnectDevice()
				text = __("Please follow these steps to proceed with the firmware update:")
				text += "\n\n" + __(
					"- Unplug your GBFlash device.\n"
					"- On your GBFlash circuit board, push and hold the small button (U22) while plugging the USB cable back in.\n"
					"- If done right, the blue LED labeled “ACT” should now keep blinking twice continuously."
				)
				text += "\n" + __("- Click OK to continue.")
				text += "\n\n" + __("Note: Illegitimate clones of the GBFlash may have been modified to disallow firmware updates.")
				msgbox = QtWidgets.QMessageBox(parent=self, icon=QtWidgets.QMessageBox.Information, windowTitle=AppInfo.NAME, text=text, standardButtons=QtWidgets.QMessageBox.Ok | QtWidgets.QMessageBox.Cancel)
				msgbox.setDefaultButton(QtWidgets.QMessageBox.Ok)
				answer = msgbox.exec()
				if answer == QtWidgets.QMessageBox.Cancel: return
			else:
				self.APP.DisconnectDevice()
				time.sleep(1)

			self.btnUpdate.setEnabled(False)
			self.btnClose.setEnabled(False)

			while True:
				ret = self.FWUPD.WriteFirmware(file_name, self.SetStatus)
				if ret == 1:
					text = __("The firmware update is complete!")
					if self.PCB_VER != "v1.3":
						text += "\n\n" + __("Please re-connect the USB cable now.")
					self.btnUpdate.setEnabled(True)
					self.btnClose.setEnabled(True)
					msgbox = QtWidgets.QMessageBox(parent=self, icon=QtWidgets.QMessageBox.Information, windowTitle=AppInfo.NAME, text=text, standardButtons=QtWidgets.QMessageBox.Ok)
					answer = msgbox.exec()
					self.DEVICE = None
					self.reject()
					return True
				elif ret == 2:
					text = __("The firmware update has failed. Please try again.")
					if (self.optFWOpen is not None and self.optFWOpen.isChecked()) or getattr(self.DEVICE, "OPEN_FW", False):
						text += "\n\n" + __("Report Open-GBFlash problems at:") + "\nhttps://github.com/Daemon125/Open-GBFlash-Firmware"
					self.btnUpdate.setEnabled(True)
					self.btnClose.setEnabled(True)
					msgbox = QtWidgets.QMessageBox(parent=self, icon=QtWidgets.QMessageBox.Critical, windowTitle=AppInfo.NAME, text=text, standardButtons=QtWidgets.QMessageBox.Ok)
					answer = msgbox.exec()
					return False
				elif ret == 3:
					text = __("The firmware update file is corrupted. Please re-install the application.")
					self.btnUpdate.setEnabled(True)
					self.btnClose.setEnabled(True)
					msgbox = QtWidgets.QMessageBox(parent=self, icon=QtWidgets.QMessageBox.Critical, windowTitle=AppInfo.NAME, text=text, standardButtons=QtWidgets.QMessageBox.Ok)
					answer = msgbox.exec()
					return False

		def SetStatus(self, text, enableUI=False, setProgress=None, cloneError=False):
			self.lblStatus.setText(__("Status: {text}", text=text))

			if cloneError:
				text = __("Your GBFlash device failed to enter the firmware update mode. This means your GBFlash may be an <b>illegitimate clone</b> that blocks certain features intentionally. If this error persists, return the device for a refund.") + "<br><br>" + text
				msgbox = QtWidgets.QMessageBox(parent=self, icon=QtWidgets.QMessageBox.Critical, windowTitle=AppInfo.NAME, text=text, standardButtons=QtWidgets.QMessageBox.Ok)
				msgbox.exec()
				return False

			if setProgress is not None:
				self.prgStatus.setValue(setProgress * 10)
			if enableUI:
				self.btnUpdate.setEnabled(True)
				self.btnClose.setEnabled(True)
			self.APP.QT_APP.processEvents()
except ImportError:
	pass
