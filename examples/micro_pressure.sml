response_ack: S1F2
<L [2]
    <A "project: ${PROJECT}">
    <A "version: ${VERSION}">
>.

response_status: S1F4
<L [1]
    <A "PRESS: ${press_data}">
>.

request_equip_status: S6F11
<L [3]
    <U4 DATAID>
    <U4 CEID>
    <L [2]
        <L [2]
            <A "CHAN1: MAX|MIN|AVG|REAL">
            <L [4]
                <F4 MAX1>
                <F4 MIN1>
                <F4 AVG1>
                <F4 REAL1>
            >
        >
        <L [2]
            <A "CHAN2: MAX|MIN|AVG|REAL">
            <L [4]
                <F4 MAX2>
                <F4 MIN2>
                <F4 AVG2>
                <F4 REAL2>
            >
        >
    >
>.

request_alarm: S5F1
<L [3]
    <U1 ALCD>
    <U2 ALID>
    <A "${ALTX}">
>.

if (S1F1) response_ack.
if (S1F3) response_status.
