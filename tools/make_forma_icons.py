#!/usr/bin/env python3
"""Original Forma SVG icons: continuous corners, glass layers and sculpted metal."""
from pathlib import Path
import math
import sys

ROOT = Path(__file__).resolve().parent.parent

def squircle(x, y, size, n=4.6):
    points=[]
    for i in range(193):
        a=2*math.pi*i/192
        c,s=math.cos(a),math.sin(a)
        points.append((x+size/2+size/2*math.copysign(abs(c)**(2/n),c),
                       y+size/2+size/2*math.copysign(abs(s)**(2/n),s)))
    return 'M'+' L'.join(f'{x:.2f},{y:.2f}' for x,y in points)+'Z'

OUTER=squircle(42,42,428)
INNER=squircle(47,47,418)

def icon(title, colors, body, defs=''):
    top,bottom=colors
    return f'''<svg xmlns="http://www.w3.org/2000/svg" width="512" height="512" viewBox="0 0 512 512" fill="none">
<title>{title}</title><desc>Original Forma artwork with layered glass, soft highlights and continuous corners.</desc>
<defs>
<linearGradient id="base" x1="256" y1="42" x2="256" y2="470" gradientUnits="userSpaceOnUse"><stop stop-color="{top}"/><stop offset="1" stop-color="{bottom}"/></linearGradient>
<linearGradient id="rim" x1="132" y1="44" x2="355" y2="470" gradientUnits="userSpaceOnUse"><stop stop-color="white" stop-opacity=".95"/><stop offset=".45" stop-color="white" stop-opacity=".18"/><stop offset=".75" stop-color="#12264B" stop-opacity=".18"/><stop offset="1" stop-color="white" stop-opacity=".55"/></linearGradient>
<linearGradient id="sheen" x1="256" y1="45" x2="256" y2="340" gradientUnits="userSpaceOnUse"><stop stop-color="white" stop-opacity=".44"/><stop offset=".55" stop-color="white" stop-opacity=".04"/><stop offset="1" stop-color="white" stop-opacity="0"/></linearGradient>
<linearGradient id="whiteGlass" x1="190" y1="135" x2="300" y2="385" gradientUnits="userSpaceOnUse"><stop stop-color="white"/><stop offset=".5" stop-color="#F1F8FF" stop-opacity=".96"/><stop offset="1" stop-color="#CEE4FB" stop-opacity=".9"/></linearGradient>
<filter id="shadow" x="-30%" y="-30%" width="160%" height="170%"><feGaussianBlur stdDeviation="9"/></filter>
<filter id="soft" x="-40%" y="-40%" width="180%" height="180%"><feGaussianBlur stdDeviation="4"/></filter>
<clipPath id="tile"><path d="{OUTER}"/></clipPath>
{defs}</defs>
<path d="{OUTER}" transform="translate(0 8)" fill="#071C38" opacity=".25" filter="url(#shadow)"/>
<path d="{OUTER}" fill="url(#base)"/>
<g><path d="{OUTER}" fill="url(#sheen)"/>{body}</g>
<path d="{OUTER}" stroke="url(#rim)" stroke-width="2.4"/>
<path d="{INNER}" stroke="white" stroke-opacity=".12" stroke-width="1"/>
</svg>'''

folder='''<linearGradient id="folderBack" x1="170" y1="132" x2="340" y2="373" gradientUnits="userSpaceOnUse"><stop stop-color="#ECFAFF"/><stop offset="1" stop-color="#6FC5FF"/></linearGradient>
<linearGradient id="folderFront" x1="256" y1="204" x2="256" y2="379" gradientUnits="userSpaceOnUse"><stop stop-color="#FFFFFF" stop-opacity=".98"/><stop offset=".52" stop-color="#E8F7FF" stop-opacity=".96"/><stop offset="1" stop-color="#B5DFFF"/></linearGradient>'''
body='''<path d="M112 176Q112 150 138 150H208Q221 150 233 165L252 188H374Q400 188 400 214V352Q400 380 372 380H140Q112 380 112 352Z" fill="#0750AA" opacity=".30" transform="translate(0 12)" filter="url(#shadow)"/>
<path d="M112 176Q112 150 138 150H207Q220 150 232 164L252 187H374Q400 187 400 213V349Q400 378 371 378H141Q112 378 112 349Z" fill="url(#folderBack)" stroke="white" stroke-opacity=".75" stroke-width="2"/>
<path d="M134 196H379V332H134Z" fill="#FFF" opacity=".9"/>
<path d="M112 230Q112 206 136 206H376Q400 206 400 230V351Q400 379 372 379H140Q112 379 112 351Z" fill="url(#folderFront)"/>
<path d="M133 208H379" stroke="white" stroke-width="3" stroke-linecap="round"/>
<path d="M143 365H369" stroke="#63A8E3" stroke-opacity=".3" stroke-width="2" stroke-linecap="round"/>
<path d="M226 246V337M227 247H295M227 286H281" stroke="#6BADE9" stroke-opacity=".25" stroke-width="14" stroke-linecap="round"/>
<path d="M225 243V334M226 244H294M226 283H280" stroke="white" stroke-opacity=".92" stroke-width="12" stroke-linecap="round"/>'''
mail_defs='''<linearGradient id="envelope" x1="150" y1="168" x2="356" y2="376" gradientUnits="userSpaceOnUse"><stop stop-color="#FFFFFF"/><stop offset=".45" stop-color="#F2FAFF"/><stop offset="1" stop-color="#C9E6FF"/></linearGradient>
<linearGradient id="envelopeFlap" x1="256" y1="196" x2="256" y2="330" gradientUnits="userSpaceOnUse"><stop stop-color="#FFFFFF"/><stop offset="1" stop-color="#DCF0FF"/></linearGradient>'''
mail_body='''<rect x="118" y="185" width="284" height="180" rx="34" fill="#073680" opacity=".32" filter="url(#shadow)" transform="translate(0 12)"/>
<rect x="116" y="172" width="280" height="184" rx="32" fill="url(#envelope)" stroke="#FFFFFF" stroke-opacity=".85" stroke-width="2.5"/>
<path d="M120 208Q122 176 158 176H354Q390 176 392 208V213L268 291Q256 299 244 291L120 213Z" fill="url(#envelopeFlap)" stroke="#8FC6EF" stroke-opacity=".55" stroke-width="2"/>
<path d="M124 344L218 268M388 344L294 268" stroke="#6FAEE3" stroke-opacity=".5" stroke-width="4" stroke-linecap="round"/>
<path d="M121 209L240 288Q256 298 272 288L391 209" stroke="white" stroke-opacity=".95" stroke-width="4.5" stroke-linecap="round" stroke-linejoin="round"/>
<path d="M133 348H240" stroke="#63A8E3" stroke-opacity=".35" stroke-width="3" stroke-linecap="round"/>'''
icons={'finder':icon('Forma Finder',('#64CAFF','#0871EC'),body,folder)}

metal='''<linearGradient id="metal" x1="140" y1="90" x2="350" y2="420" gradientUnits="userSpaceOnUse"><stop stop-color="#FFFFFF"/><stop offset=".24" stop-color="#D9DEE7"/><stop offset=".47" stop-color="#FAFCFF"/><stop offset=".64" stop-color="#A4ADBD"/><stop offset=".85" stop-color="#E6EBF3"/><stop offset="1" stop-color="#8A96A9"/></linearGradient>
<linearGradient id="innerMetal" x1="170" y1="170" x2="340" y2="347" gradientUnits="userSpaceOnUse"><stop stop-color="#747E91"/><stop offset=".48" stop-color="#D6DCE6"/><stop offset="1" stop-color="#FFFFFF"/></linearGradient>
<radialGradient id="hub"><stop stop-color="#8C98AB"/><stop offset=".72" stop-color="#56647A"/><stop offset="1" stop-color="#344259"/></radialGradient>'''
pts=[]
for i in range(12):
 for off,r in [(-.5,133),(-.28,133),(-.23,159),(.23,159),(.28,133),(.5,133)]:
  a=(i+off)*math.tau/12-math.pi/2
  pts.append((256+r*math.cos(a),256+r*math.sin(a)))
gear='M'+' L'.join(f'{x:.2f},{y:.2f}' for x,y in pts)+'Z'
body=f'''<circle cx="256" cy="267" r="159" fill="#12203C" opacity=".3" filter="url(#shadow)"/>
<circle cx="256" cy="256" r="169" fill="#727D90" opacity=".22"/>
<circle cx="256" cy="256" r="167" stroke="white" stroke-opacity=".5" stroke-width="2"/>
<path d="{gear}" fill="url(#metal)" stroke="#F8FAFF" stroke-width="2" stroke-linejoin="round"/>
<circle cx="256" cy="256" r="107" fill="url(#innerMetal)"/>
<circle cx="256" cy="256" r="94" fill="url(#hub)" stroke="#3E4C62" stroke-width="2"/>
<circle cx="256" cy="256" r="73" fill="none" stroke="#CED8E7" stroke-opacity=".42" stroke-width="6"/>
<circle cx="256" cy="256" r="49" fill="url(#metal)" stroke="white" stroke-opacity=".6" stroke-width="2"/>
<circle cx="256" cy="256" r="26" fill="url(#innerMetal)"/>
<path d="M184 161A121 121 0 0 1 336 167" stroke="white" stroke-opacity=".75" stroke-width="3" stroke-linecap="round"/>'''
icons['system_settings']=icon('Forma Settings',('#D6DCE7','#98A3B8'),body,metal)

screen='''<linearGradient id="screen" x1="256" y1="93" x2="256" y2="418" gradientUnits="userSpaceOnUse"><stop stop-color="#26323E"/><stop offset=".45" stop-color="#101C28"/><stop offset="1" stop-color="#080E19"/></linearGradient>
<linearGradient id="screenRim" x1="100" y1="92" x2="405" y2="420" gradientUnits="userSpaceOnUse"><stop stop-color="#FFFFFF"/><stop offset=".35" stop-color="#ACB6C8"/><stop offset=".65" stop-color="#677589"/><stop offset="1" stop-color="#E2E9F5"/></linearGradient>'''
body='''<rect x="79" y="90" width="354" height="334" rx="56" fill="#14223B" opacity=".35" filter="url(#shadow)" transform="translate(0 9)"/>
<rect x="77" y="83" width="358" height="346" rx="58" fill="url(#screenRim)"/>
<rect x="84" y="90" width="344" height="332" rx="52" fill="url(#screen)" stroke="#1C293D" stroke-width="2"/>
<path d="M94 141Q94 100 140 100H372Q415 100 418 140V230Q256 179 94 230Z" fill="white" opacity=".045"/>
<path d="M144 179L217 247L144 315" stroke="#000" stroke-width="25" stroke-linecap="round" stroke-linejoin="round" opacity=".35" transform="translate(0 5)"/>
<path d="M144 175L217 243L144 311" stroke="url(#whiteGlass)" stroke-width="24" stroke-linecap="round" stroke-linejoin="round"/>
<path d="M258 316H357" stroke="url(#whiteGlass)" stroke-width="23" stroke-linecap="round"/>
<path d="M151 168L224 236" stroke="white" stroke-width="2" stroke-linecap="round" opacity=".55"/>'''
icons['terminal']=icon('Forma Terminal',('#D4DCE8','#8D9BB1'),body,screen)
icons['mail']=icon('Forma Mail',('#5FCCFF','#0B6FE8'),mail_body,mail_defs)

body='''<rect x="79" y="90" width="354" height="334" rx="56" fill="#14223B" opacity=".35" filter="url(#shadow)" transform="translate(0 9)"/>
<rect x="77" y="83" width="358" height="346" rx="58" fill="url(#screenRim)"/>
<rect x="84" y="90" width="344" height="332" rx="52" fill="url(#screen)" stroke="#1C293D" stroke-width="2"/>
<g stroke="#5B8C8C" stroke-width="1.5" opacity=".23"><path d="M127 105V407M170 94V419M213 92V420M256 92V420M299 92V420M342 94V419M385 105V407M98 141H414M87 185H425M87 229H425M87 273H425M87 317H425M98 361H414"/></g>
<path d="M101 280H161L193 225L227 332L274 151L311 292L339 250H411" stroke="#58FFAC" stroke-width="24" opacity=".16" stroke-linecap="round" stroke-linejoin="round" filter="url(#soft)"/>
<path d="M101 280H161L193 225L227 332L274 151L311 292L339 250H411" stroke="#72F3B0" stroke-width="9" stroke-linecap="round" stroke-linejoin="round"/>
<path d="M101 278H161L193 223" stroke="#D6FFE7" stroke-width="2" stroke-linecap="round" opacity=".8"/>
<path d="M94 141Q94 100 140 100H372Q415 100 418 140V217Q256 178 94 217Z" fill="white" opacity=".055"/>'''
icons['task_manager']=icon('Forma Task Manager',('#D4DCE8','#8D9BB1'),body,screen)

photos_defs='''<linearGradient id="photoCard" x1="190" y1="150" x2="330" y2="380" gradientUnits="userSpaceOnUse"><stop stop-color="#FFFFFF"/><stop offset=".55" stop-color="#F6FBFF"/><stop offset="1" stop-color="#CFE7FF"/></linearGradient>
<linearGradient id="skyOne" x1="200" y1="160" x2="330" y2="300" gradientUnits="userSpaceOnUse"><stop stop-color="#57BEFF"/><stop offset="1" stop-color="#BEE8FF"/></linearGradient>
<linearGradient id="skyTwo" x1="200" y1="160" x2="330" y2="300" gradientUnits="userSpaceOnUse"><stop stop-color="#FFA46B"/><stop offset="1" stop-color="#FFD9A6"/></linearGradient>
<linearGradient id="hillOne" x1="256" y1="250" x2="256" y2="330" gradientUnits="userSpaceOnUse"><stop stop-color="#4FD873"/><stop offset="1" stop-color="#1E9C46"/></linearGradient>
<linearGradient id="hillTwo" x1="256" y1="250" x2="256" y2="330" gradientUnits="userSpaceOnUse"><stop stop-color="#2FB980"/><stop offset="1" stop-color="#116E3B"/></linearGradient>
<clipPath id="cardClip"><rect x="150" y="146" width="236" height="196" rx="22"/></clipPath>
<clipPath id="backClip"><rect x="128" y="172" width="236" height="196" rx="22"/></clipPath>'''
photos_body='''<rect x="128" y="172" width="236" height="196" rx="24" fill="#0A3A80" opacity=".30" filter="url(#shadow)" transform="translate(0 12)"/>
<g transform="rotate(-9 256 270)">
  <rect x="128" y="172" width="236" height="196" rx="24" fill="url(#photoCard)" stroke="#FFFFFF" stroke-opacity=".8" stroke-width="2"/>
  <g clip-path="url(#backClip)">
    <rect x="128" y="172" width="236" height="196" fill="url(#skyTwo)"/>
    <circle cx="300" cy="222" r="26" fill="#FFD60A" opacity=".95"/>
    <path d="M112 380L214 262L268 330L310 282L404 380Z" fill="url(#hillTwo)"/>
    <path d="M112 380Q214 306 286 336T404 322V380Z" fill="url(#hillOne)"/>
  </g>
</g>
<g transform="rotate(6 256 262)">
  <rect x="150" y="146" width="236" height="196" rx="24" fill="#0A3A80" opacity=".22" filter="url(#shadow)" transform="translate(0 8)"/>
  <rect x="150" y="146" width="236" height="196" rx="24" fill="url(#photoCard)" stroke="#FFFFFF" stroke-opacity=".9" stroke-width="2.5"/>
  <g clip-path="url(#cardClip)">
    <rect x="150" y="146" width="236" height="196" fill="url(#skyOne)"/>
    <circle cx="322" cy="196" r="24" fill="#FFD60A"/>
    <path d="M136 350L234 240L288 306L330 258L420 350Z" fill="url(#hillTwo)"/>
    <path d="M136 350Q234 280 304 308T420 294V350Z" fill="url(#hillOne)"/>
  </g>
  <path d="M168 160H248" stroke="white" stroke-opacity=".75" stroke-width="3" stroke-linecap="round"/>
</g>'''
icons['photos']=icon('Forma Photos',('#66CCFF','#0B71EC'),photos_body+'\n'+photos_defs)

video_defs='''<linearGradient id="filmBody" x1="180" y1="120" x2="200" y2="400" gradientUnits="userSpaceOnUse"><stop stop-color="#FFFFFF"/><stop offset=".5" stop-color="#F0F7FF"/><stop offset="1" stop-color="#C7E1FB"/></linearGradient>
<linearGradient id="frame" x1="200" y1="140" x2="200" y2="380" gradientUnits="userSpaceOnUse"><stop stop-color="#22324A"/><stop offset="1" stop-color="#0B1420"/></linearGradient>
<radialGradient id="playDisc" cx=".38" cy=".34" r=".75" gradientUnits="objectBoundingBox"><stop stop-color="#FFFFFF"/><stop offset=".62" stop-color="#EDF6FF"/><stop offset="1" stop-color="#BBDCFA"/></radialGradient>'''
video_body='''<rect x="122" y="118" width="268" height="286" rx="34" fill="#081A33" opacity=".32" filter="url(#shadow)" transform="translate(0 12)"/>
<rect x="118" y="104" width="268" height="286" rx="32" fill="url(#filmBody)" stroke="#FFFFFF" stroke-opacity=".85" stroke-width="2.5"/>
<g stroke="#0B2140" stroke-opacity=".68" stroke-width="4"><path d="M148 110V384M356 110V384"/></g>
<g fill="#0B2140" opacity=".30">
  <rect x="125" y="122" width="15" height="22" rx="4"/><rect x="125" y="164" width="15" height="22" rx="4"/>
  <rect x="125" y="206" width="15" height="22" rx="4"/><rect x="125" y="248" width="15" height="22" rx="4"/>
  <rect x="125" y="290" width="15" height="22" rx="4"/><rect x="125" y="332" width="15" height="22" rx="4"/>
  <rect x="364" y="122" width="15" height="22" rx="4"/><rect x="364" y="164" width="15" height="22" rx="4"/>
  <rect x="364" y="206" width="15" height="22" rx="4"/><rect x="364" y="248" width="15" height="22" rx="4"/>
  <rect x="364" y="290" width="15" height="22" rx="4"/><rect x="364" y="332" width="15" height="22" rx="4"/>
</g>
<rect x="153" y="136" width="166" height="222" rx="18" fill="url(#frame)" stroke="#A9C8E8" stroke-opacity=".55" stroke-width="2"/>
<path d="M160 146Q160 139 168 139H304Q310 139 310 146V188Q236 168 160 188Z" fill="white" opacity=".10"/>
<circle cx="236" cy="247" r="54" fill="url(#playDisc)" stroke="#FFFFFF" stroke-width="2"/>
<circle cx="236" cy="247" r="54" fill="none" stroke="#8FB9E0" stroke-opacity=".5" stroke-width="1.5"/>
<path d="M222 215Q222 205 231 210L273 240Q281 246 273 252L231 282Q222 287 222 277Z" fill="#1B6FD6"/>
<path d="M153 370H320" stroke="#63A8E3" stroke-opacity=".35" stroke-width="3" stroke-linecap="round"/>'''
icons['video_player']=icon('Forma Video Player',('#5A8CFF','#1B2A6B'),video_body,video_defs)

text_defs='''<linearGradient id="paper" x1="170" y1="110" x2="337" y2="401" gradientUnits="userSpaceOnUse"><stop stop-color="#FFFFFF"/><stop offset=".6" stop-color="#F5FAFF"/><stop offset="1" stop-color="#D3E6FA"/></linearGradient>
<linearGradient id="pen" x1="310" y1="165" x2="338" y2="183" gradientUnits="userSpaceOnUse"><stop stop-color="#A6DDFF"/><stop offset=".25" stop-color="#4EADFF"/><stop offset=".65" stop-color="#1874DB"/><stop offset="1" stop-color="#0C459D"/></linearGradient>
<linearGradient id="nib" x1="282" y1="305" x2="309" y2="326" gradientUnits="userSpaceOnUse"><stop stop-color="#FFFFFF"/><stop offset=".5" stop-color="#D8E3F1"/><stop offset="1" stop-color="#8398B4"/></linearGradient>'''
text_body='''<rect x="132" y="110" width="242" height="296" rx="30" fill="#253B65" opacity=".24" filter="url(#shadow)" transform="translate(0 10)"/>
<rect x="128" y="104" width="242" height="296" rx="28" fill="url(#paper)" stroke="white" stroke-opacity=".9" stroke-width="2.5"/>
<path d="M153 120H340" stroke="white" stroke-width="3" stroke-linecap="round"/>
<path d="M170 165H310M170 198H299M170 231H310M170 264H265M170 297H247M170 330H230" stroke="#7499BD" stroke-opacity=".64" stroke-width="8" stroke-linecap="round"/>
<path d="M170 367H236" stroke="#93B5D5" stroke-opacity=".5" stroke-width="3" stroke-linecap="round"/>
<g transform="rotate(33 324 256)">
 <rect x="307" y="126" width="35" height="221" rx="12" fill="#173C6A" opacity=".25" filter="url(#soft)" transform="translate(7 7)"/>
 <rect x="305" y="120" width="34" height="204" rx="12" fill="url(#pen)" stroke="#D7EDFF" stroke-width="1.5"/>
 <path d="M305 149H339V166H305Z" fill="url(#nib)"/>
 <path d="M310 173V300" stroke="#D5F2FF" stroke-opacity=".8" stroke-width="3" stroke-linecap="round"/>
 <path d="M305 310H339L330 347L322 362L314 347Z" fill="url(#nib)" stroke="#A1B5CB" stroke-width="1.2"/>
 <path d="M322 325V351" stroke="#617992" stroke-width="1.5"/>
 <path d="M318 353L322 364L326 353Z" fill="#253A53"/>
 <path d="M332 127V145" stroke="white" stroke-opacity=".7" stroke-width="2" stroke-linecap="round"/>
</g>'''
icons['text_editor']=icon('Forma Text Editor',('#CEE9FF','#72A9E7'),text_body,text_defs)

# App collection: four individually legible tiles, one lifted out of the grid.
store_defs=r"""<linearGradient id="cyanTile" x1="150" y1="140" x2="244" y2="244" gradientUnits="userSpaceOnUse"><stop stop-color="#A3F6FF"/><stop offset="1" stop-color="#35BDD9"/></linearGradient>
<linearGradient id="coralTile" x1="270" y1="128" x2="368" y2="230" gradientUnits="userSpaceOnUse"><stop stop-color="#FFE0A8"/><stop offset="1" stop-color="#FF8A77"/></linearGradient>
<linearGradient id="violetTile" x1="150" y1="266" x2="246" y2="364" gradientUnits="userSpaceOnUse"><stop stop-color="#E0D7FF"/><stop offset="1" stop-color="#9F8AF5"/></linearGradient>
<linearGradient id="whiteTile" x1="270" y1="266" x2="366" y2="364" gradientUnits="userSpaceOnUse"><stop stop-color="#FFFFFF"/><stop offset="1" stop-color="#C8E5FF"/></linearGradient>"""
store_body=r"""<g fill="#071C48" opacity=".35" filter="url(#shadow)" transform="translate(0 12)">
<rect x="143" y="145" width="102" height="102" rx="27"/>
<rect x="265" y="132" width="102" height="102" rx="27" transform="rotate(10 316 183)"/>
<rect x="143" y="267" width="102" height="102" rx="27"/>
<rect x="265" y="267" width="102" height="102" rx="27"/></g>
<rect x="143" y="145" width="102" height="102" rx="27" fill="url(#cyanTile)"/>
<rect x="143" y="267" width="102" height="102" rx="27" fill="url(#violetTile)"/>
<rect x="265" y="267" width="102" height="102" rx="27" fill="url(#whiteTile)"/>
<g transform="rotate(10 316 183)"><rect x="265" y="132" width="102" height="102" rx="27" fill="url(#coralTile)"/>
<path d="M316 155Q320 179 344 183Q320 187 316 211Q312 187 288 183Q312 179 316 155Z" fill="white" fill-opacity=".95"/>
<path d="M283 141H344" stroke="white" stroke-opacity=".5" stroke-width="2" stroke-linecap="round"/></g>
<g stroke="white" stroke-opacity=".4" stroke-width="2" stroke-linecap="round"><path d="M163 154H225"/><path d="M163 276H225"/><path d="M285 276H347"/></g>"""
icons['app_store']=icon('Forma App Store',('#27629F','#152C62'),store_body,store_defs)

screenshot_defs='''<linearGradient id="captureFrame" x1="148" y1="157" x2="368" y2="359" gradientUnits="userSpaceOnUse"><stop stop-color="#FFFFFF"/><stop offset=".54" stop-color="#F1FAFF"/><stop offset="1" stop-color="#B8DDF8"/></linearGradient>
<linearGradient id="captureSky" x1="171" y1="181" x2="342" y2="322" gradientUnits="userSpaceOnUse"><stop stop-color="#254BC6"/><stop offset=".48" stop-color="#638AF2"/><stop offset="1" stop-color="#94DFF0"/></linearGradient>
<linearGradient id="captureMountain" x1="200" y1="238" x2="302" y2="329" gradientUnits="userSpaceOnUse"><stop stop-color="#A4FAF0"/><stop offset="1" stop-color="#1DA4CE"/></linearGradient>
<linearGradient id="captureForeground" x1="238" y1="264" x2="337" y2="337" gradientUnits="userSpaceOnUse"><stop stop-color="#E1C8FF"/><stop offset="1" stop-color="#849BEE"/></linearGradient>
<linearGradient id="captureBadge" x1="314" y1="308" x2="379" y2="378" gradientUnits="userSpaceOnUse"><stop stop-color="#FFFFFF"/><stop offset="1" stop-color="#D1E9FF"/></linearGradient>
<clipPath id="captureImage"><rect x="150" y="171" width="212" height="166" rx="16"/></clipPath>'''
screenshot_body='''<rect x="134" y="157" width="244" height="198" rx="29" fill="#07347D" opacity=".32" filter="url(#shadow)" transform="translate(0 12)"/>
<rect x="134" y="155" width="244" height="198" rx="28" fill="url(#captureFrame)" stroke="white" stroke-opacity=".9" stroke-width="2.4"/>
<rect x="150" y="171" width="212" height="166" rx="16" fill="url(#captureSky)"/>
<g clip-path="url(#captureImage)">
 <circle cx="307" cy="212" r="18" fill="#DDF9FF" fill-opacity=".92"/>
 <path d="M131 338L215 229Q222 222 229 229L326 337Z" fill="url(#captureMountain)"/>
 <path d="M218 337L295 260Q302 254 310 261L379 337Z" fill="url(#captureForeground)"/>
 <path d="M150 185Q252 171 362 216V239Q246 198 150 212Z" fill="white" fill-opacity=".09"/>
</g>
<rect x="150" y="171" width="212" height="166" rx="16" stroke="#528CCB" stroke-opacity=".35" stroke-width="1.6"/>
<path d="M145 161H353" stroke="white" stroke-opacity=".85" stroke-width="2" stroke-linecap="round"/>
<g fill="none" stroke-linecap="round" stroke-linejoin="round">
 <path d="M151 126H106V171M361 126H406V171M106 337V382H151M406 337V382H361" stroke="#064B93" stroke-opacity=".28" stroke-width="12" transform="translate(0 4)"/>
 <path d="M151 123H106V168M361 123H406V168M106 334V379H151M406 334V379H361" stroke="url(#whiteGlass)" stroke-width="10"/>
 <path d="M151 120H106V165M361 120H406V165" stroke="white" stroke-opacity=".8" stroke-width="2"/>
</g>
<rect x="306" y="303" width="84" height="76" rx="24" fill="#134B8B" opacity=".3" filter="url(#soft)" transform="translate(0 5)"/>
<rect x="304" y="300" width="84" height="76" rx="23" fill="url(#captureBadge)" stroke="white" stroke-width="2"/>
<g fill="none" stroke="#2478C9" stroke-width="4.5" stroke-linecap="round">
 <circle cx="346" cy="338" r="17"/>
 <path d="M346 312V324M346 352V364M320 338H332M360 338H372"/>
</g>
<circle cx="346" cy="338" r="3" fill="#2478C9"/>'''
icons['screenshot']=icon('Forma Screenshot',('#56DAE9','#2364CF'),screenshot_body,screenshot_defs)

auth_defs='''<linearGradient id="authSteel" x1="192" y1="116" x2="326" y2="270" gradientUnits="userSpaceOnUse"><stop stop-color="#FFFFFF"/><stop offset=".28" stop-color="#E4EBF1"/><stop offset=".56" stop-color="#A3B4C2"/><stop offset=".8" stop-color="#F6FAFF"/><stop offset="1" stop-color="#8195A6"/></linearGradient>
<linearGradient id="authBody" x1="173" y1="235" x2="330" y2="378" gradientUnits="userSpaceOnUse"><stop stop-color="#FFFFFF"/><stop offset=".52" stop-color="#FFF9E9"/><stop offset="1" stop-color="#EED9B1"/></linearGradient>
<linearGradient id="authKey" x1="248" y1="278" x2="266" y2="337" gradientUnits="userSpaceOnUse"><stop stop-color="#B96A21"/><stop offset="1" stop-color="#DB9F47"/></linearGradient>
<linearGradient id="authShield" x1="325" y1="303" x2="393" y2="403" gradientUnits="userSpaceOnUse"><stop stop-color="#5CE2D4"/><stop offset="1" stop-color="#0C8E91"/></linearGradient>'''
auth_body='''<rect x="150" y="235" width="212" height="153" rx="34" fill="#9A4B18" opacity=".32" filter="url(#shadow)" transform="translate(0 11)"/>
<path d="M192 241V191C192 104 320 104 320 191V241" stroke="#A45E28" stroke-opacity=".2" stroke-width="32" transform="translate(0 5)"/>
<path d="M192 241V191C192 104 320 104 320 191V241" stroke="url(#authSteel)" stroke-width="29" stroke-linecap="round"/>
<path d="M184 224V191C184 145 216 119 251 119" stroke="white" stroke-opacity=".8" stroke-width="3" stroke-linecap="round"/>
<rect x="149" y="229" width="214" height="151" rx="33" fill="url(#authBody)" stroke="white" stroke-opacity=".9" stroke-width="2.5"/>
<path d="M177 237H335" stroke="white" stroke-opacity=".95" stroke-width="3" stroke-linecap="round"/>
<path d="M183 368H326" stroke="#C09A60" stroke-opacity=".28" stroke-width="2" stroke-linecap="round"/>
<circle cx="256" cy="290" r="16" fill="url(#authKey)"/>
<path d="M248 300H264L270 333Q270 338 265 338H247Q242 338 242 333Z" fill="url(#authKey)"/>
<path d="M329 303L361 291L394 303V340Q394 378 361 397Q329 378 329 340Z" fill="#8A511E" opacity=".24" filter="url(#soft)" transform="translate(0 6)"/>
<path d="M325 300L359 287L394 300V339Q394 377 359 397Q325 377 325 339Z" fill="url(#authShield)" stroke="#E7FFFB" stroke-opacity=".9" stroke-width="2.5" stroke-linejoin="round"/>
<path d="M338 339L353 354L380 324" stroke="white" stroke-width="8" stroke-linecap="round" stroke-linejoin="round"/>
<path d="M333 305L359 295L384 305" stroke="white" stroke-opacity=".45" stroke-width="2" stroke-linecap="round"/>'''
icons['polkit_agent']=icon('Forma Authentication',('#FFD780','#ED8A31'),auth_body,auth_defs)

for folder,svg in icons.items():
    if len(sys.argv) > 1 and folder not in sys.argv[1:]:
        continue
    data=ROOT/'applications'/folder/'data'
    existing=list(data.glob('*.svg')) if data.is_dir() else []
    if not existing:
        # New apps name their icon after their desktop id.
        desktop_ids={'app_store':'io.github.skjsbsnq.Forma.AppStore',
                     'screenshot':'io.github.skjsbsnq.Forma.Screenshot',
                     'polkit_agent':'io.github.skjsbsnq.Forma.PolkitAgent'}
        target=data/desktop_ids.get(folder, folder+'.svg')
    else:
        target=existing[0]
    data.mkdir(parents=True, exist_ok=True)
    target.write_text(svg+'\n')
