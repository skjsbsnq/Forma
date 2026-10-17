#!/usr/bin/env python3
"""Real HTTPS exchange against a localhost provider; never opens a real account."""
import base64,hashlib,http.server,json,os,pathlib,ssl,subprocess,tempfile,threading,urllib.parse,urllib.request,sys
state={'bad':False,'pkce':False,'refresh':False,'polls':0}
class Handler(http.server.BaseHTTPRequestHandler):
 def log_message(self,*args):pass
 def answer(self,data,status=200):
  raw=json.dumps(data).encode();self.send_response(status);self.send_header('Content-Type','application/json');self.send_header('Content-Length',str(len(raw)));self.end_headers();self.wfile.write(raw)
 def do_GET(self):
  q=urllib.parse.parse_qs(urllib.parse.urlsplit(self.path).query)
  state['challenge']=q['code_challenge'][0]
  redirect=q['redirect_uri'][0]
  try:urllib.request.urlopen(redirect+'?'+urllib.parse.urlencode({'state':'WRONG','code':'attack'}),timeout=4)
  except urllib.error.HTTPError as e:state['bad']=e.code==400
  self.send_response(302);self.send_header('Location',redirect+'?'+urllib.parse.urlencode({'state':q['state'][0],'code':'local-code'}));self.end_headers()
 def do_POST(self):
  q=urllib.parse.parse_qs(self.rfile.read(int(self.headers['Content-Length'])).decode())
  if self.path=='/device':self.answer({'device_code':'local-device','user_code':'TEST-CODE','verification_uri':'https://login.example.invalid/device','expires_in':60,'interval':1});return
  grant=q['grant_type'][0]
  if grant=='authorization_code':
   digest=base64.urlsafe_b64encode(hashlib.sha256(q['code_verifier'][0].encode()).digest()).rstrip(b'=').decode();state['pkce']=digest==state['challenge'];assert q['code']==['local-code']
  if grant=='refresh_token':
   state['refresh']=q['refresh_token']==['local-refresh'];self.answer({'access_token':'local-refreshed','refresh_token':'local-rotated','expires_in':3600});return
  if grant=='urn:ietf:params:oauth:grant-type:device_code':
   state['polls']+=1
   if state['polls']==1:self.answer({'error':'authorization_pending'},400);return
  self.answer({'access_token':'local-access','refresh_token':'local-refresh','expires_in':3600,'token_type':'Bearer'})
class Server(http.server.ThreadingHTTPServer):
 def handle_error(self, request, client_address):
  if isinstance(sys.exc_info()[1], (BrokenPipeError, ConnectionResetError, ssl.SSLError)): return
  super().handle_error(request, client_address)
with tempfile.TemporaryDirectory(prefix='forma-oauth-') as path:
 p=pathlib.Path(path);subprocess.run(['openssl','req','-x509','-newkey','rsa:2048','-nodes','-keyout',str(p/'key'),'-out',str(p/'cert'),'-days','1','-subj','/CN=localhost','-addext','subjectAltName=DNS:localhost'],check=True,stdout=subprocess.DEVNULL,stderr=subprocess.DEVNULL)
 server=Server(('127.0.0.1',0),Handler);ctx=ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER);ctx.load_cert_chain(p/'cert',p/'key');server.socket=ctx.wrap_socket(server.socket,server_side=True);threading.Thread(target=server.serve_forever,daemon=True).start()
 binary=os.environ.get('FORMA_OAUTH_TEST_BINARY') or pathlib.Path(__file__).resolve().parents[2]/'build/applications/mail/forma-mail-oauth-tests'
 env=os.environ|{'SSL_CERT_FILE':str(p/'cert'),'FORMA_OAUTH_TEST_BASE':f'https://localhost:{server.server_port}'}
 try:subprocess.run([str(binary)],env=env,check=True,timeout=30)
 finally:server.shutdown()
 assert state['bad'] and state['pkce'] and state['refresh'] and state['polls']>=2,state
 print('Local OAuth provider verified PKCE, CSRF-state rejection, refresh rotation and device pending handling.')
