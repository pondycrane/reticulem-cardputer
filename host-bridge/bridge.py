"""
ReticuleM Host Bridge

Connects the M5Stack Cardputer over USB serial (or TCP) to a local
Reticulum / LXMF node.  Frames between host and device are JSON
objects, one per line, newline-terminated.

Frame types from Cardputer -> Host:
  { "type": "hello", "identity": "..." }
  { "type": "tx_msg", "to": "<lxmf hash>", "body": "...", "from": "..." }
  { "type": "req_contacts" }
  { "type": "req_status" }
  { "type": "add_contact", "name": "...", "hash": "..." }

Frame types from Host -> Cardputer:
  { "type": "rx_msg", "from": "<hash>", "body": "...", "title": "...", "time": <unix_ms> }
  { "type": "ack", "id": <msg_id>, "status": "delivered|propagated|failed" }
  { "type": "contact", "name": "...", "hash": "..." }
  { "type": "status", "connected": true, "peers": 3 }

Usage:
    source ~/reticulum-venv/bin/activate
    python3 bridge.py --serial /dev/ttyACM0 --lxmf-identity cardputer_owner
"""
import argparse
import asyncio
import json
import logging
import os
import sys
import time
from typing import Optional

try:
    import serial_asyncio
except ImportError:
    import warnings
    warnings.warn("pyserial-asyncio not installed; install with: pip install pyserial-asyncio")
import RNS
import LXMF

logging.basicConfig(
    level=logging.INFO,
    format="%(asctime)s [%(levelname)s] %(message)s"
)
logger = logging.getLogger("reticulem_bridge")


class ReticuleMBridge:
    """
    Main bridge class: cardputer (serial/tcp) <-> LXMF router
    """
    def __init__(
        self,
        serial_port: Optional[str],
        baudrate: int,
        tcp_port: Optional[int],
        lxmf_identity_name: str,
        lxmf_storage: str,
    ):
        self.serial_port = serial_port
        self.baudrate = baudrate
        self.tcp_port = tcp_port
        self.lxmf_identity_name = lxmf_identity_name
        self.lxmf_storage = lxmf_storage

        # Reticulum / LXMF
        self.rns_identity: Optional[RNS.Identity] = None
        self.lxm_router: Optional[LXMF.LXMRouter] = None
        self.own_destination = None

        # Serial state
        self.serial_writer: Optional[asyncio.StreamWriter] = None
        self.serial_transport = None

        # Outstanding sends: tx_msg_id -> { future, timestamp }
        self.pending: dict[str, dict] = {}
        self.msg_counter = 0

        # Contact cache
        self.contacts: dict[str, str] = {}  # hash -> name

    # ------------------------------------------------------------------
    # Reticulum / LXMF init
    # ------------------------------------------------------------------
    def start_lxmf(self):
        logger.info("Starting Reticulum/LXMF...")
        RNS.Reticulum()

        # Load or create identity
        identity_path = os.path.join(self.lxmf_storage, "identity")
        os.makedirs(self.lxmf_storage, exist_ok=True)
        if os.path.isfile(identity_path):
            with open(identity_path, "rb") as f:
                self.rns_identity = RNS.Identity.from_bytes(f.read())
            logger.info("Loaded existing identity")
        else:
            self.rns_identity = RNS.Identity()
            with open(identity_path, "wb") as f:
                f.write(self.rns_identity.get_private_key())
            logger.info("Created new identity")

        self.lxm_router = LXMF.LXMRouter(
            identity=self.rns_identity,
            storagepath=self.lxmf_storage,
        )

        # Create an LXMF destination that clients can send to
        self.own_destination = self.lxm_router.register_delivery_identity(
            self.rns_identity,
            display_name=self.lxmf_identity_name,
        )
        self.lxm_router.register_delivery_callback(self.on_lxmf_delivery)
        logger.info(
            f"LXMF destination: {RNS.prettyhexrep(self.own_destination.hash)}"
        )

        # Announce our presence periodically
        self.lxm_router.announce(self.own_destination.hash)

    def on_lxmf_delivery(self, message: LXMF.LXMessage):
        """Called when an LXMF message arrives for us."""
        src_hash = RNS.prettyhexrep(message.source_hash)
        body = message.content_as_string()
        title = message.title if hasattr(message, 'title') else ""
        logger.info(f"LXMF rx from {src_hash}: {body[:80]}...")

        # Forward to Cardputer
        frame = {
            "type": "rx_msg",
            "from": src_hash,
            "body": body,
            "title": title,
            "time": int(time.time() * 1000),
        }
        asyncio.get_event_loop().call_soon_threadsafe(
            self._send_frame_to_device, frame
        )

    # ------------------------------------------------------------------
    # Serial / TCP transport
    # ------------------------------------------------------------------
    async def open_serial(self):
        if not self.serial_port:
            logger.info("No serial port configured")
            return
        logger.info(f"Opening serial {self.serial_port} @ {self.baudrate}")
        self.serial_reader = serial_asyncio.open_serial_connection(
            url=self.serial_port,
            baudrate=self.baudrate,
        )
        reader, writer = await self.serial_reader
        self.serial_writer = writer
        asyncio.create_task(self._serial_read_loop(reader))

    async def _serial_read_loop(self, reader: asyncio.StreamReader):
        """Read newline-delimited JSON frames from Cardputer."""
        buf = b""
        while True:
            try:
                chunk = await reader.read(1024)
                if not chunk:
                    logger.warning("Serial closed")
                    break
                buf += chunk
                while b"\n" in buf:
                    line, buf = buf.split(b"\n", 1)
                    line = line.strip()
                    if line:
                        self._handle_device_frame(line.decode("utf-8", "replace"))
            except Exception as e:
                logger.error(f"Serial read error: {e}")
                await asyncio.sleep(1)

    async def open_tcp(self):
        if not self.tcp_port:
            return
        server = await asyncio.start_server(
            self._tcp_client_connected, "0.0.0.0", self.tcp_port
        )
        logger.info(f"TCP server listening on :{self.tcp_port}")
        async with server:
            await server.serve_forever()

    async def _tcp_client_connected(
        self, reader: asyncio.StreamReader, writer: asyncio.StreamWriter
    ):
        peer = writer.get_extra_info("peername")
        logger.info(f"TCP client connected: {peer}")
        # For now, only one concurrent TCP client is supported
        self.serial_writer = writer
        buf = b""
        try:
            while True:
                chunk = await reader.read(1024)
                if not chunk:
                    break
                buf += chunk
                while b"\n" in buf:
                    line, buf = buf.split(b"\n", 1)
                    line = line.strip()
                    if line:
                        self._handle_device_frame(line.decode("utf-8", "replace"))
        except Exception as e:
            logger.error(f"TCP client error: {e}")
        finally:
            logger.info(f"TCP client disconnected: {peer}")

    def _send_frame_to_device(self, frame: dict):
        """Queue a JSON frame to the Cardputer."""
        payload = json.dumps(frame, separators=(",", ":")) + "\n"
        data = payload.encode("utf-8")
        if self.serial_writer:
            try:
                self.serial_writer.write(data)
                # Note: do not await drain here; this may be called from sync context
            except Exception as e:
                logger.warning(f"Write to device failed: {e}")

    # ------------------------------------------------------------------
    # Protocol handlers
    # ------------------------------------------------------------------
    def _handle_device_frame(self, line: str):
        try:
            doc = json.loads(line)
        except json.JSONDecodeError:
            logger.warning(f"Bad JSON from device: {line[:120]}")
            return

        ftype = doc.get("type", "unknown")
        logger.debug(f"Device frame: {ftype}")

        if ftype == "hello":
            identity = doc.get("identity", "anonymous")
            logger.info(f"Device hello: {identity}")
            self._send_frame_to_device(
                {"type": "status", "ready": True, "identity": self.lxmf_identity_name}
            )

        elif ftype == "tx_msg":
            asyncio.create_task(self._do_tx_msg(doc))

        elif ftype == "req_contacts":
            for h, name in self.contacts.items():
                self._send_frame_to_device({"type": "contact", "name": name, "hash": h})

        elif ftype == "req_status":
            self._send_frame_to_device(
                {
                    "type": "status",
                    "connected": True,
                    "identity": RNS.prettyhexrep(self.own_destination.hash),
                    "peers": 0,  # TODO: expose from router
                }
            )

        elif ftype == "add_contact":
            name = doc.get("name", "")
            h = doc.get("hash", "")
            if name and h:
                self.contacts[h] = name
                logger.info(f"Added contact {name} -> {h}")

        else:
            logger.warning(f"Unknown frame type from device: {ftype}")

    async def _do_tx_msg(self, doc: dict):
        to_hash = doc.get("to", "")
        body = doc.get("body", "")
        if not to_hash or not body:
            return

        logger.info(f"Sending LXMF -> {to_hash}: {body[:80]}...")

        # Resolve destination
        try:
            destination_hash = bytes.fromhex(to_hash.strip().replace("<", "").replace(">", ""))
        except ValueError:
            logger.error(f"Invalid destination hash: {to_hash}")
            return

        # Create LXMF destination
        dest = RNS.Destination(
            None,
            RNS.Destination.OUT,
            RNS.Destination.SINGLE,
            "lxmf",
            "delivery",
            destination_hash,
        )

        lxm = LXMF.LXMessage(
            dest,
            self.own_destination,
            content=body,
            title=doc.get("title", ""),
            desired_method=LXMF.LXMessage.DIRECT,
        )

        # Add tracking state
        msg_id = str(self.msg_counter)
        self.msg_counter += 1
        self.pending[msg_id] = {
            "doc": doc,
            "lxm": lxm,
            "t0": time.time(),
        }

        override = doc.get("method", "direct")
        if override == "propagated":
            lxm.desired_method = LXMF.LXMessage.PROPAGATED

        # Register outcome callbacks
        def on_delivered(lxm):
            logger.info(f"Message {msg_id} delivered")
            self._send_frame_to_device(
                {"type": "ack", "id": msg_id, "status": "delivered"}
            )
            self.pending.pop(msg_id, None)

        def on_failed(lxm):
            logger.warning(f"Message {msg_id} failed")
            self._send_frame_to_device(
                {"type": "ack", "id": msg_id, "status": "failed"}
            )
            self.pending.pop(msg_id, None)

        lxm.register_delivery_callback(on_delivered)
        lxm.register_failed_callback(on_failed)

        self.lxm_router.handle_outbound(lxm)

    # ------------------------------------------------------------------
    # Run loop
    # ------------------------------------------------------------------
    async def run(self):
        self.start_lxmf()
        tasks = []
        if self.serial_port:
            tasks.append(asyncio.create_task(self.open_serial()))
        if self.tcp_port:
            tasks.append(asyncio.create_task(self.open_tcp()))
        if not tasks:
            logger.error("No transport configured (use --serial and/or --tcp-port)")
            sys.exit(1)
        await asyncio.gather(*tasks)


def main():
    parser = argparse.ArgumentParser(description="ReticuleM Host Bridge")
    parser.add_argument("--serial", default="/dev/ttyACM0", help="Serial port")
    parser.add_argument("--baud", type=int, default=115200, help="Serial baudrate")
    parser.add_argument("--tcp-port", type=int, default=None, help="TCP listen port")
    parser.add_argument(
        "--lxmf-identity",
        default="cardputer_user",
        help="Display name for LXMF identity",
    )
    parser.add_argument(
        "--lxmf-storage",
        default="~/.reticulem/lxmf",
        help="LXMF storage directory",
    )
    args = parser.parse_args()

    lxmf_storage = os.path.expanduser(args.lxmf_storage)

    bridge = ReticuleMBridge(
        serial_port=args.serial,
        baudrate=args.baud,
        tcp_port=args.tcp_port,
        lxmf_identity_name=args.lxmf_identity,
        lxmf_storage=lxmf_storage,
    )
    try:
        asyncio.run(bridge.run())
    except KeyboardInterrupt:
        logger.info("Shutting down")


if __name__ == "__main__":
    main()
