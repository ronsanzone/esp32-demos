"""Read the board's USB serial for N seconds, surviving USB re-enumeration on reset."""
import sys, time, serial
port, secs = sys.argv[1] if len(sys.argv) > 1 else '/dev/cu.usbmodem101', float(sys.argv[2]) if len(sys.argv) > 2 else 8
end, buf = time.time() + secs, b''
while time.time() < end:
    try:
        s = serial.Serial(port, 115200, timeout=0.3)
        while time.time() < end: buf += s.read(4096)
    except (serial.SerialException, OSError): time.sleep(0.2)
sys.stdout.write(buf.decode(errors='replace'))
