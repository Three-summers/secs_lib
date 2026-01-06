/*
Sample.SML
ITRI/MIRL 2001/09/14
Author: Liaw Shen-Tai
Email: 880731@itri.org.tw or stliaw@ms7.url.com.tw
Tel: 886-3-5916723
*/

S1F0.

s1f1_1: S1F1 W
<L
  <L
    <Boolean 0x01 0x00>
    <F4 0.5567>
    <L
      <F8 0.9>
    >
  >
  <B 0x00 0x05 0x06 0x09 0xff>
>.

s1f1_2: S1F1 W
<L
  <L
    <Boolean 0x01 0x00>
    <F4 0.5568>
    <L
      <F8 0.9>
    >
  >
  <B 0x00 0x05 0x06 0x09 0xff>
>.

s1f2_1: S1F2
<L
  <A "Hi Good Idea!">
  <A "Are you Okay!">
  <L
    <A "Level">
    <A "Another">
    <Boolean 0x01 0x00>
    <F4 0.5567>
    <L
      <A "T">
      <F8 0.9>
    >
    <A "Hello">
  >
  <B 0x00 0x05 0x06 0x09 0xff>
>.

s1f2_2: S1F2
<L
  <A "Hi Good Idea!">
  <A "Are you Okay!">
>.

s1f3_1: S1F3 W
<L
    <U2 1>
>.

s1f3_2: S1F3 W
<L
  <U2 5>
>.

s1f4_1: S1F4
<L
  <I2 1>
>.

s1f4_2: S1F4
<L
  <I2 5>
>.

StatusTank1 :'S1F3' W
    <L
       < U2  3001 >
       < U2  3002 >
       < U2  3003 >
       < U2  3004 >
       < U2  3005 >
       < U2  3006 >
       < U2  3007 >
       < U2  3008 >
    >.

StatusTank2 :'S1F3' W
    <L
       < U2  3021 >
       < U2  3022 >
       < U2  3023 >
       < U2  3024 >
       < U2  3025 >
       < U2  3026 >
       < U2  3027 >
       < U2  3028 >
    >.

StatusQDR1 :'S1F3' W
    <L
       < U2  3041 >
       < U2  3042 >
       < U2  3043 >
       < U2  3044 >
       < U2  3045 >
       < U2  3046 >
       < U2  3047 >
       < U2  3048 >
    >.

StatusQDR2 :'S1F3' W
    <L
       < U2  3061 >
       < U2  3062 >
       < U2  3063 >
       < U2  3064 >
       < U2  3065 >
       < U2  3066 >
       < U2  3067 >
       < U2  3068 >
    >.

StatusTankTime1 :'S1F3' W
    <L
       < U2  3101 >
       < U2  3102 >
       < U2  3103 >
    >.
StatusTankTime2 :'S1F3' W
    <L
       < U2  3111 >
       < U2  3112 >
       < U2  3113 >
    >.

StatusQDRTime :'S1F3' W
    <L
       < U2  3121 >
       < U2  3122 >
       < U2  3123 >
       < U2  3124 >
    >.
StatusTime :'S1F3' W
    <L
       < U2  3200 >
       < U2  3211 >
       < U2  3212 >
       < U2  3213 >
       < U2  3214 >
    >.

StatusLamp :'S1F3' W
    <L
       < U2  3249 >
       < U2  3250 >
       < U2  3251 >
       < U2  3252 >
       < U2  3253 >
       < U2  3254 >
       < U2  3255 >
       < U2  3256 >
    >.

S1F5 W
<B 0x01>.

S1F6
<L
  <I2>
  <I2>
>.

S1F13:'S1F13' W
     <L
        <A "WET.01">
        <A "REV.01">
     >.

S1F14:'S1F14'
    <L
        <B 0x00>
        <L
           <A "WET.01">
           <A "REV.01">
        >
     > .
                   
UNKNOWFUCNTION:'S1F15'
    <L
        <B 0x00>
        <L
           <A "WET.01">
           <A "REV.01">
        >
     > .

S2F21 W
<U1 1>.

S2F22
<U1 0>.

S2F41 W
<L [2]
  <A "RCMD">
  <L
    <L [2]
      <A "CPNAME1">
      <A "CPVAL1">
    >
    <L [2]
      <A "CPNAME2">
      <A "CPVAL2">
    >
  >
>.

SELECTRCP1:'S2F41' W
   <L 
      <U2 5001>
      <L
        <L
          <A  'PPID'>
          <A 'RECOVER.RCP'>
        >
      >
   >.

SELECTRCP2:'S2F41' W
   <L 
      <U2 5001>
      <L
        <L
          <A  'PPID'>
          <A 'PARTICLE.RCP'>
        >
      >
   >.

SELECTNORCP:'S2F41' W
   <L 
      <U2 5001>
      <L
        <L
          <A  'PPID'>
          <A  'TESTNO.RCP'>
        >
      >
   >.

LOADCONFIRM:'S2F41' W
   <L 
      <U2 5002>
      <L>
   >.

STARTPROCESS:'S2F41' W
   <L 
      <U2 5003>
      <L>
   >.
STOPPROCESS:'S2F41' W
   <L 
      <U2 5004>
      <L>
   >.

SYSTEMON:'S2F41' W
   <L 
      <U2 5005>
      <L>
   >.

SYSTEMOFF:'S2F41' W
   <L 
      <U2 5006>
      <L>
   >.

SWITCHLOCAL:'S2F41' W
   <L 
      <U2 5020>
      <L>
   >.

SWITCHREMOTE:'S2F41' W
   <L 
      <U2 5021>
      <L>
   >.

S2F42
<L
  <B 0x00>
  <L>
>.

S5F1: S5F1 W
<L [3]
  <B 0x06>
  <U2 10>
  <A "Fire Alarm">
>.

S5F2
<B 0x00>.

S15F31 W
<A "RECIPE000">.

S15F32
<L[4]
  <A "RECIPE000">
  <L[2]
    <L[2]
      <A "ATTRIB1">
      <U1 5>
    >
    <L[2]
      <A "ATTRIB2">
      <U2 300>
    >
  >
  <B
    0x01 0x02 0x03 0x04 0x05 0xFF 0xFF 0xFF 0xFF 0xFF
    0xFF 0xFF 0xFF 0xFF 0xFF 0xFF 0xFF 0xFF 0xFF 0xFF
    0xFF 0xFF 0xFF 0xFF 0xFF 0xFF 0xFF 0xFF 0xFF 0xFF
    0xFF 0xFF 0xFF 0xFF 0xFF 0xFF 0xFF 0xFF 0xFF 0xFF
    0xFF 0xFF 0xFF 0xFF 0xFF 0xFF 0xFF 0xFF 0xFF 0xFF
    0xFF 0xFF 0xFF 0xFF 0xFF 0xFF 0xFF 0xFF 0xFF 0xFF
    0xFF 0xFF 0xFF 0xFF 0xFF 0xFF 0xFF 0xFF 0xFF 0xFF
    0xFF 0xFF 0xFF 0xFF 0xFF 0xFF 0xFF 0xFF 0xFF 0xFF
    0xFF 0xFF 0xFF 0xFF 0xFF 0xFF 0xFF 0xFF 0xFF 0xFF
    0xFF 0xFF 0xFF 0xFF 0xFF 0xFF 0xFF 0xFF 0xFF 0xFF
    0xFF 0xFF 0xFF 0xFF 0xFF 0xFF 0xFF 0xFF 0xFF 0xFF
    0xFF 0xFF 0xFF 0xFF 0xFF 0xFF 0xFF 0xFF 0xFF 0xFF
    0xFF 0xFF 0xFF 0xFF 0xFF 0xFF 0xFF 0xFF 0xFF 0xFF
    0xFF 0xFF 0xFF 0xFF 0xFF 0xFF 0xFF 0xFF 0xFF 0xFF
    0xFF 0xFF 0xFF 0xFF 0xFF 0xFF 0xFF 0xFF 0xFF 0xFF
    0xFF 0xFF 0xFF 0xFF 0xFF 0xFF 0xFF 0xFF 0xFF 0xFF
    0xFF 0xFF 0xFF 0xFF 0xFF 0xFF 0xFF 0xFF 0xFF 0xFF
    0xFF 0xFF 0xFF 0xFF 0xFF 0xFF 0xFF 0xFF 0xFF 0xFF
    0xFF 0xFF 0xFF 0xFF 0xFF 0xFF 0xFF 0xFF 0xFF 0xFF
    0xFF 0xFF 0xFF 0xFF 0xFF 0xFF 0xFF 0xFF 0xFF 0xFF
    0xFF 0xFF 0xFF 0xFF 0xFF 0xFF 0xFF 0xFF 0xFF 0xFF
    0xFF 0xFF 0xFF 0xFF 0xFF 0xFF 0xFF 0xFF 0xFF 0xFF
    0xFF 0xFF 0xFF 0xFF 0xFF 0xFF 0xFF 0xFF 0xFF 0xFF
    0xFF 0xFF 0xFF 0xFF 0xFF 0xFF 0xFF 0xFF 0xFF 0xFF
    0xFF 0xFF 0xFF 0xFF 0xFF 0xFF 0xFF 0xFF 0xFF 0xFF
    0xFF 0xFF 0xFF 0xFF 0xFF 0xFF 0xFF 0xFF 0xFF 0xFF
    0xFF 0xFF 0xFF 0xFF 0xFF 0xFF 0xFF 0xFF 0xFF 0xFF
    0xFF 0xFF 0xFF
  >
  <L
    <U1 20>
    <L
      <L
        <U1 22>
        <A "TEST1">
      >
      <L
        <U1 33>
        <A "TEST2">
      >
    >
  >
>.

if (s1f1 (4)==<F4 0.5567>) s1f2_1.
if (s1f1 (4)==<F4 0.5568>) s1f2_2.
if (s1f1) s1f3_1.
if (s1f3 (2)==<U2 1>) s1f4_1.
if (s1f3 (2)==<U2 5>) s1f4_2.
if (s2f21) s2f22.
if (s15f31) s15f32.

every 5 send s1f1_1.
every 10 send s15f31.
