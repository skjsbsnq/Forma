#!/usr/bin/env python3
"""Offline integration test. All IMAP/SMTP traffic stays on localhost with a temporary CA."""
import base64, os, socket, ssl, subprocess, tempfile, threading, pathlib, re

raw=(b'From: Sender <sender@example.invalid>\r\nSubject: =?UTF-8?B?5Lit5paH?=\r\n'
     b'Date: Sat, 17 Oct 2026 10:00:00 +0000\r\nContent-Type: text/plain; charset=UTF-8\r\n'
     b'Content-Transfer-Encoding: base64\r\n\r\n5L2g5aW9\r\n')
commands=[]
sent=[]
errors=[]

def listener(context, protocol):
    server=socket.socket(); server.bind(('127.0.0.1',0)); server.listen(); server.settimeout(1)
    port=server.getsockname()[1]
    def serve():
        while not stop.is_set():
            try: conn,_=server.accept()
            except socket.timeout: continue
            except OSError: break
            def handle(conn):
                try:
                    with context.wrap_socket(conn,server_side=True) as sock:
                        stream=sock.makefile('rwb',buffering=0)
                        def send(s): stream.write(s.encode())
                        if protocol=='imap':
                            send('* OK local IMAP test server\r\n')
                            while line:=stream.readline():
                                s=line.decode().strip(); commands.append(s)
                                tag,_,cmd=s.partition(' '); upper=cmd.upper()
                                if upper.startswith('CAPABILITY'): send('* CAPABILITY IMAP4rev1 AUTH=PLAIN AUTH=XOAUTH2 MOVE UIDPLUS\r\n'+tag+' OK capability\r\n')
                                elif upper.startswith('AUTHENTICATE'):
                                    send('+ \r\n'); data=stream.readline().strip()
                                    if upper.startswith('AUTHENTICATE XOAUTH2'): assert b'auth=Bearer local-access' in base64.b64decode(data)
                                    send(tag+' OK authenticated\r\n')
                                elif upper.startswith('LOGIN'): send(tag+' OK logged in\r\n')
                                elif upper.startswith('LIST'): send('* LIST (\\HasNoChildren) "/" "INBOX"\r\n* LIST (\\Trash) "/" "Trash"\r\n'+tag+' OK list\r\n')
                                elif upper.startswith(('SELECT','EXAMINE')): send('* 1 EXISTS\r\n* OK [UIDVALIDITY 123] valid\r\n'+tag+' OK [READ-WRITE] select\r\n')
                                elif upper.startswith('UID SEARCH'): send('* SEARCH 1\r\n'+tag+' OK search\r\n')
                                elif upper.startswith('UID FETCH') and 'BODY.PEEK[]' in upper:
                                    send('* 1 FETCH (UID 1 BODY[] {'+str(len(raw))+'}\r\n'); stream.write(raw);send(')\r\n'+tag+' OK fetch\r\n')
                                elif upper.startswith('UID FETCH'): send('* 1 FETCH (UID 1 FLAGS ())\r\n'+tag+' OK flags\r\n')
                                elif upper.startswith(('UID STORE','UID MOVE')): send(tag+' OK changed\r\n')
                                elif upper.startswith('LOGOUT'): send('* BYE\r\n'+tag+' OK logout\r\n'); break
                                else: send(tag+' BAD unsupported\r\n')
                        else:
                            send('220 localhost test SMTP\r\n')
                            while line:=stream.readline():
                                cmd=line.decode().strip();commands.append(cmd);upper=cmd.upper()
                                if upper.startswith('EHLO'): send('250-localhost\r\n250 AUTH PLAIN XOAUTH2\r\n')
                                elif upper.startswith(('AUTH PLAIN','AUTH XOAUTH2')):
                                    data=cmd.split()[2].encode() if len(cmd.split())>2 else None
                                    if data is None: send('334 \r\n');data=stream.readline().strip()
                                    if upper.startswith('AUTH XOAUTH2'): assert b'auth=Bearer local-access' in base64.b64decode(data)
                                    send('235 authenticated\r\n')
                                elif upper.startswith(('MAIL FROM:','RCPT TO:')): send('250 OK\r\n')
                                elif upper=='DATA':
                                    send('354 send content\r\n');data=b''
                                    while chunk:=stream.readline():
                                        if chunk==b'.\r\n':break
                                        data+=chunk
                                    sent.append(data);send('250 queued locally\r\n')
                                elif upper=='QUIT': send('221 bye\r\n');break
                                else: send('500 unsupported\r\n')
                except (BrokenPipeError,ConnectionResetError,ssl.SSLError): pass
                except Exception as exc: errors.append(repr(exc))
            threading.Thread(target=handle,args=(conn,),daemon=True).start()
        server.close()
    threading.Thread(target=serve,daemon=True).start()
    return port

with tempfile.TemporaryDirectory(prefix='forma-mail-protocol-') as temp:
    temp=pathlib.Path(temp); stop=threading.Event()
    subprocess.run(['openssl','req','-x509','-newkey','rsa:2048','-nodes','-keyout',str(temp/'key.pem'),'-out',str(temp/'cert.pem'),'-days','1','-subj','/CN=localhost','-addext','subjectAltName=DNS:localhost'],check=True,stdout=subprocess.DEVNULL,stderr=subprocess.DEVNULL)
    ctx=ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER);ctx.load_cert_chain(temp/'cert.pem',temp/'key.pem')
    imap=listener(ctx,'imap');smtp=listener(ctx,'smtp')
    env=os.environ|{'SSL_CERT_FILE':str(temp/'cert.pem'),'XDG_CONFIG_HOME':str(temp/'config'),'FORMA_TEST_IMAP_PORT':str(imap),'FORMA_TEST_SMTP_PORT':str(smtp)}
    binary=pathlib.Path(__file__).resolve().parents[2]/'build/applications/mail/forma-mail-tests'
    untrusted=subprocess.run([str(binary)],env=env|{'SSL_CERT_FILE':'/etc/ssl/cert.pem'},text=True,capture_output=True,timeout=45)
    assert untrusted.returncode != 0, "Untrusted server certificate was accepted"
    assert not sent, "Untrusted test must never send email"
    result=subprocess.run([str(binary)],env=env,text=True,capture_output=True,timeout=45)
    print(result.stdout); print(result.stderr)
    oauth_result=subprocess.run([str(binary)],env=env|{'FORMA_TEST_XOAUTH':'1','XDG_CONFIG_HOME':str(temp/'oauth-config')},text=True,capture_output=True,timeout=45)
    print(oauth_result.stdout); print(oauth_result.stderr)
    stop.set()
    if oauth_result.returncode: print(commands); raise SystemExit(oauth_result.returncode)
    if result.returncode or errors: print('server errors:',errors); print('commands:',commands); raise SystemExit(result.returncode or 1)
    assert len(sent)==2 and b'Content-Transfer-Encoding: base64' in sent[0]
    assert any('AUTHENTICATE XOAUTH2' in c for c in commands)
    assert any('AUTH XOAUTH2' in c for c in commands)
    assert any('BODY.PEEK[]' in c for c in commands)
    assert any('UID STORE 1 +FLAGS.SILENT (\\Seen)' in c for c in commands)
    assert any('UID MOVE 1 "Trash"' in c for c in commands)
    # Passwords must never be present in the settings file.
    for p in (temp/'config').rglob('settings.conf'): assert 'local-test-only' not in p.read_text()
    print('Verified TLS, PEEK fetch, remote flags, MOVE, SMTP payload and no plaintext credential persistence.')
