# webtransportd

webtransportd lets a web browser exchange real-time data with a program you write — in any language — without that program needing to know anything about networking.

You write a normal little program that reads standard input and writes standard output. webtransportd does the hard part: it speaks the browser's WebTransport protocol (QUIC, TLS, and HTTP/3), and for each browser that connects it launches your program and pipes bytes both ways — what the browser sends arrives on your program's stdin, and whatever your program prints goes back to the browser.

It's like [websocketd](https://github.com/joewalnes/websocketd), but for the newer WebTransport — so you also get WebTransport's two delivery modes: reliable streams and unreliable datagrams.
