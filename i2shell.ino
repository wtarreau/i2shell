/*
 * i2shell: shell-friendly usb-to-i2c gateway -- 2022-01-23 - Willy Tarreau
 * Presents a ttyACM device on an ATTINY85. Requires TinyWireM and DigiCDC.
 *
 * Input commands:
 *    S <addr>  : start / resync to idle for address <addr>
 *    W <byte>* : send byte(s) to addr above
 *    P         : stop (end of transmission)
 *    R <num>   : read <num> bytes
 * Spaces are ignored but delimit values. Values are in hex.
 * Some devices (e.g. DS1307) implement addressable registers which can be seen
 * as a sub-address. These require a write followed by one (or multiple) reads.
 * Each command implicitly ends the previous one.
 * E.g:
 *   - initialise DS1307:
 *     echo "S68W0 0P" | socat - /dev/ttyACM1,rawer
 *   - read time on DS1307:
 *     echo "S 68 W 0 R 7" | socat - /dev/ttyACM1,rawer
 *     35 05 00 01 01 01 00
 */
#include <TinyWireM.h>
#include <DigiCDC.h>

enum state {
  ST_CMD  = 0, // parsing command
  ST_ADDR = 1, // parsing address
  ST_CMDW = 2, // addr+w sent, parsing byte to send
  ST_CMDR = 3, // addr+r sent, parsing byte count to recv
  ST_RECV = 4, // receiving requested bytes
};

char buf[9] = "00000000";
unsigned char ret = 0;
unsigned char addr = 0;
unsigned char data = 0;
unsigned char digits = 0;
unsigned char to_print = 0;
enum state state = ST_CMD;

/* read char <c> and update previous hex value <in> with it. Return
 * 1 if hex char was read, 0 otherwise. <in> may be NULL to only check
 * for a hex digit.
 */
static unsigned char read_hex(unsigned char *in, char c)
{
  if (c >= '0' && c <= '9')
    c -= '0';
  else if (c >= 'A' && c <= 'F')
    c -= 'A' - 10;
  else if (c >= 'a' && c <= 'z')
    c -= 'a' - 10;
  else
    return 0; // unknown char, unchanged
  if (in)
    *in = (*in << 4) + c;
  return 1;
}

/* returns 0-9/A-Z for input values 0..35 */
static unsigned char tohex(unsigned char in)
{
  return (in < 10) ? in + '0' : in - 10 + 'A';
}

static void print_hex(const char *pfx, unsigned char x, const char *sfx)
{
  if (pfx && *pfx)
    SerialUSB.write(pfx);
  SerialUSB.write(tohex(x >> 4));
  SerialUSB.write(tohex(x & 0xF));
  if (sfx && *sfx)
    SerialUSB.write(sfx);
}

static void print_help()
{
#ifndef NOHELP
  SerialUSB.write(
    "?       : print this help\r\n"
    "S<addr> : set I2C addr (hex)\r\n"
    "W<byte>*: write bytes (hex)\r\n"
    "P       : push (end of write)\r\n"
    "R<num>  : read bytes (hex)\r\n"
    "\r\n");
#endif
}

void loop()
{
  unsigned char l;

  if (SerialUSB.available() < 1) {
    /* it seems the the only way to make transfers reliable is to send a byte
     * when there are too few input contents. Let's send a CR/LF after CR/LF,
     * otherwise a space that will be ignored on outputs at least. Other chars
     * would either corrupt output on each block boundary (8 bytes) or make it
     * painful to work in interactive mode.
     */
    if (to_print) {
      SerialUSB.write(to_print);
      to_print = 0;
    }
    return;
  }

  l = SerialUSB.read();

  to_print = (l == '\r' || l == '\n') ? '\r' : ' ';

  /* turn to upper */
  if ((unsigned char)(l - 'a') <= 'z' - 'a')
    l -= 'a' - 'A';

  switch (state) {
    case ST_CMD: {
    parse_cmd:
      data = digits = 0;
      if (l == 'S') {
        state = ST_ADDR;
        addr = 0; // resync
      }
      else if (l == 'W') {
        TinyWireM.beginTransmission(addr);
        state = ST_CMDW;
      }
      else if (l == 'R') {
        state = ST_CMDR;
      }
      else if (l == '?') {
        print_help();
        to_print = 0;
      }
      // ignore other unknown commands (e.g. spaces)
      break;
    }

    case ST_ADDR: {
      read_hex(&addr, l);
      if (l == 'S' || l == 'P') {
        state = ST_ADDR;
        addr = 0; // resync
      }

      if (l != 'W' && l != 'R' && l != '?')
        break;

      state = ST_CMD;
      goto parse_cmd;
    }

    case ST_CMDW: {
      ret = read_hex(&data, l);
      digits += ret;
      if ((digits && !ret) || digits == 2) {
        TinyWireM.send(data);
        data = digits = 0;
      }
      if (!ret && l != ' ' && l != 'W') {
        ret = TinyWireM.endTransmission();
        if (ret) {
          print_hex("W!", ret, "\n");
          to_print = 0;
        }
        state = ST_CMD;
        goto parse_cmd;
      }
      break;
    }

    case ST_CMDR: {
      ret = read_hex(&data, l);
      digits += ret;
      if (!ret && digits) {
        // OK we have at least one digit and possibly a delimiter.
        ret = TinyWireM.requestFrom(addr, data);
        if (ret) {
          print_hex("R!", ret, "\n");
          to_print = 0;
        }
        data = digits = 0;
        state = ST_RECV;
        goto recv_now;
      }

      if (!ret && l != ' ' && l != '\n') {
        state = ST_CMD;
        goto parse_cmd;
      }
      break;
    }

    case ST_RECV: {
      recv_now:
      while (TinyWireM.available()) {
        data = TinyWireM.receive();
        print_hex(NULL, data, " ");
      }
      SerialUSB.write("\r\n");
      to_print = 0;
      state = ST_CMD;
      goto parse_cmd;
    }
  } /* end of switch/case */
}

void setup()
{
  SerialUSB.begin();
  TinyWireM.begin();
}
