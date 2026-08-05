# FamiCAS dual USB CDC bridge

The RP2040 exposes two independent Windows COM ports so ROM programming data
and FC debug output can never be mixed.

## CDC 0: ROM writer

```text
ROMWRITER_Host <-> USB CDC 0 <-> UART0 <-> ROMWRITER_handmade
                                      GPIO12 TX
                                      GPIO13 RX
                                      921600 baud, 8N1
```

CDC 0 is a byte-for-byte binary bridge. It adds no prefixes, line endings, or
startup messages. Select this COM port in `ROMWRITER_Host`.

## CDC 1: FC printf

```text
FC $4016 bit 0 -> expansion connector -> GPIO9 (UART1 RX) -> USB CDC 1
                                                        300000 baud, 8N1
```

CDC 1 is receive-only. Open it in a terminal at 300000 baud. GPIO8 is not
configured as TX.

## Build output

```text
build/2PORTmonitor.uf2
```
