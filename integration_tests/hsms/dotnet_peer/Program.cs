using Secs4Net;
using Microsoft.Extensions.Options;
using System.Buffers;
using System.Buffers.Binary;
using System.IO;
using static Secs4Net.Item;

namespace HsmsPeer;

internal static class Program
{
    private const ushort ControlSessionId = 0xFFFF;
    private const string DefaultIp = "127.0.0.1";

    private sealed class SilentLogger : ISecsGemLogger
    {
        public void MessageIn(SecsMessage msg, int id) { }
        public void MessageOut(SecsMessage msg, int id) { }
        public void Debug(string msg) { }
        public void Info(string msg) { }
        public void Warning(string msg) { }
        public void Error(string msg) { }
        public void Error(string msg, Exception ex) { }
        public void Error(string msg, SecsMessage? message, Exception? ex) { }
    }

    private sealed class S9F1WatchLogger : ISecsGemLogger
    {
        private readonly TaskCompletionSource _sent = new(TaskCreationOptions.RunContinuationsAsynchronously);

        public async Task WaitSentAsync(CancellationToken ct)
        {
            using var reg = ct.Register(() => _sent.TrySetCanceled(ct));
            await _sent.Task.ConfigureAwait(false);
        }

        public void MessageIn(SecsMessage msg, int id) { }
        public void MessageOut(SecsMessage msg, int id)
        {
            if (msg.S == 9 && msg.F == 1)
            {
                _sent.TrySetResult();
            }
        }
        public void Debug(string msg) { }
        public void Info(string msg) { }
        public void Warning(string msg) { }
        public void Error(string msg) { }
        public void Error(string msg, Exception ex) { }
        public void Error(string msg, SecsMessage? message, Exception? ex) { }
    }

    private readonly record struct HsmsFrame(
        ushort SessionId,
        byte HeaderByte2,
        byte HeaderByte3,
        byte PType,
        MessageType SType,
        uint SystemBytes,
        byte[] Body)
    {
        public byte[] Encode()
        {
            var length = 10 + (uint)Body.Length;
            var buf = new byte[4 + length];

            BinaryPrimitives.WriteUInt32BigEndian(buf.AsSpan(0, 4), length);
            BinaryPrimitives.WriteUInt16BigEndian(buf.AsSpan(4, 2), SessionId);
            buf[6] = HeaderByte2;
            buf[7] = HeaderByte3;
            buf[8] = PType;
            buf[9] = (byte)SType;
            BinaryPrimitives.WriteUInt32BigEndian(buf.AsSpan(10, 4), SystemBytes);
            Body.CopyTo(buf.AsSpan(14));
            return buf;
        }
    }

    private static Item MakeTestItem()
        => L(U4(123u), A("HELLO"), L(U1((byte)1, (byte)2, (byte)3)));

    private static byte[] EncodeItem(Item item)
    {
        var w = new ArrayBufferWriter<byte>();
        item.EncodeTo(w);
        return w.WrittenSpan.ToArray();
    }

    private static async Task ReadExactlyAsync(Stream input, Memory<byte> buffer, CancellationToken ct)
    {
        var offset = 0;
        while (offset < buffer.Length)
        {
            var n = await input.ReadAsync(buffer[offset..], ct);
            if (n <= 0)
            {
                throw new EndOfStreamException("对端关闭");
            }
            offset += n;
        }
    }

    private static async Task<HsmsFrame> ReadFrameAsync(Stream input, CancellationToken ct)
    {
        var lenBytes = new byte[4];
        await ReadExactlyAsync(input, lenBytes, ct);
        var length = BinaryPrimitives.ReadUInt32BigEndian(lenBytes);
        if (length < 10)
        {
            throw new InvalidDataException($"非法 HSMS length: {length}");
        }

        var payload = new byte[length];
        await ReadExactlyAsync(input, payload, ct);

        var sessionId = BinaryPrimitives.ReadUInt16BigEndian(payload.AsSpan(0, 2));
        var hb2 = payload[2];
        var hb3 = payload[3];
        var pType = payload[4];
        var sType = (MessageType)payload[5];
        var systemBytes = BinaryPrimitives.ReadUInt32BigEndian(payload.AsSpan(6, 4));
        var body = payload.AsSpan(10).ToArray();

        return new HsmsFrame(sessionId, hb2, hb3, pType, sType, systemBytes, body);
    }

    private static async Task WriteFrameAsync(Stream output, HsmsFrame frame, CancellationToken ct)
    {
        var bytes = frame.Encode();
        await output.WriteAsync(bytes, ct);
        await output.FlushAsync(ct);
    }

    private static (byte stream, byte function, bool wBit) ParseDataHeader(byte hb2, byte hb3)
    {
        var wBit = (hb2 & 0b1000_0000) != 0;
        var stream = (byte)(hb2 & 0b0111_1111);
        return (stream, hb3, wBit);
    }

    private static async Task WaitSelectedAsync(ISecsConnection connection, CancellationToken ct)
    {
        if (connection.State == ConnectionState.Selected)
        {
            return;
        }

        var tcs = new TaskCompletionSource(TaskCreationOptions.RunContinuationsAsynchronously);
        void Handler(object? _, ConnectionState state)
        {
            if (state == ConnectionState.Selected)
            {
                tcs.TrySetResult();
            }
        }

        connection.ConnectionChanged += Handler;
        try
        {
            if (connection.State == ConnectionState.Selected)
            {
                return;
            }

            using var reg = ct.Register(() => tcs.TrySetCanceled(ct));
            await tcs.Task.ConfigureAwait(false);
        }
        finally
        {
            connection.ConnectionChanged -= Handler;
        }
    }

    private static async Task<int> TcpEchoServerAsync(string ip, int port, ushort deviceId, CancellationToken ct)
    {
        var opts = Options.Create(new SecsGemOptions
        {
            DeviceId = deviceId,
            IsActive = false,
            IpAddress = ip,
            Port = port,
            T3 = 5000,
            T5 = 2000,
            T6 = 5000,
            T7 = 5000,
            T8 = 2000,
            LinkTestInterval = 200,
        });

        var logger = new SilentLogger();
        await using var connection = new HsmsConnection(opts, logger)
        {
            // echo-server 只需要“被动端可正确响应 SELECT/LINKTEST + 收到主消息并回包”；
            // 自动 LINKTEST 会在大包场景引入额外控制事务与超时干扰，因此这里禁用。
            LinkTestEnabled = false,
        };
        using var secsGem = new SecsGem(opts, connection, logger);

        connection.Start(ct);
        await WaitSelectedAsync(connection, ct).ConfigureAwait(false);

        await foreach (var wrapper in secsGem.GetPrimaryMessageAsync(ct).WithCancellation(ct).ConfigureAwait(false))
        {
            var msg = wrapper.PrimaryMessage;
            if (msg.S != 1 || msg.F != 13 || !msg.ReplyExpected)
            {
                Console.Error.WriteLine($"收到非预期主消息: {msg}");
                return 2;
            }

            // 最小验证：对端可发送，且本端可成功解码 Item。
            if (msg.SecsItem is not null)
            {
                _ = EncodeItem(msg.SecsItem);
            }

            var rsp = new SecsMessage(1, 14, replyExpected: false)
            {
                SecsItem = msg.SecsItem,
            };
            await wrapper.TryReplyAsync(rsp, ct).ConfigureAwait(false);
            // 避免过早 Dispose/Shutdown 导致大包回包在对端尚未完全接收时被中断。
            await Task.Delay(TimeSpan.FromMilliseconds(200), ct).ConfigureAwait(false);
            return 0;
        }

        Console.Error.WriteLine("未收到主消息");
        return 3;
    }

    private static async Task<int> TcpReorderEchoServerAsync(string ip, int port, ushort deviceId, CancellationToken ct)
    {
        var opts = Options.Create(new SecsGemOptions
        {
            DeviceId = deviceId,
            IsActive = false,
            IpAddress = ip,
            Port = port,
            T3 = 5000,
            T5 = 2000,
            T6 = 5000,
            T7 = 5000,
            T8 = 2000,
            LinkTestInterval = 200,
        });

        var logger = new SilentLogger();
        await using var connection = new HsmsConnection(opts, logger)
        {
            // 乱序回包用例需要更稳定的时序：避免自动 LINKTEST 抢占控制流与触发 Retry。
            LinkTestEnabled = false,
        };
        using var secsGem = new SecsGem(opts, connection, logger);

        connection.Start(ct);
        await WaitSelectedAsync(connection, ct).ConfigureAwait(false);

        PrimaryMessageWrapper? first = null;
        await foreach (var wrapper in secsGem.GetPrimaryMessageAsync(ct).WithCancellation(ct).ConfigureAwait(false))
        {
            var msg = wrapper.PrimaryMessage;
            if (msg.S != 1 || msg.F != 13 || !msg.ReplyExpected)
            {
                Console.Error.WriteLine($"收到非预期主消息: {msg}");
                return 2;
            }

            if (first is null)
            {
                first = wrapper;
                continue;
            }

            static async Task<bool> ReplyEchoAsync(PrimaryMessageWrapper w, CancellationToken c)
            {
                var m = w.PrimaryMessage;
                var rsp = new SecsMessage(m.S, (byte)(m.F + 1), replyExpected: false)
                {
                    SecsItem = m.SecsItem,
                };
                return await w.TryReplyAsync(rsp, c).ConfigureAwait(false);
            }

            // 固定乱序：先回第二条，再回第一条，确保对端必须按 system-bytes 匹配。
            if (!await ReplyEchoAsync(wrapper, ct).ConfigureAwait(false))
            {
                Console.Error.WriteLine("第二条消息回包失败（重复回包）");
                return 3;
            }
            if (!await ReplyEchoAsync(first, ct).ConfigureAwait(false))
            {
                Console.Error.WriteLine("第一条消息回包失败（重复回包）");
                return 4;
            }

            await Task.Delay(TimeSpan.FromMilliseconds(200), ct).ConfigureAwait(false);
            return 0;
        }

        Console.Error.WriteLine("未收到足够的主消息（至少 2 条）");
        return 5;
    }

    private static async Task<int> TcpS9F1ServerAsync(string ip, int port, ushort deviceId, CancellationToken ct)
    {
        var opts = Options.Create(new SecsGemOptions
        {
            DeviceId = deviceId,
            IsActive = false,
            IpAddress = ip,
            Port = port,
            T3 = 5000,
            T5 = 2000,
            T6 = 5000,
            T7 = 5000,
            T8 = 2000,
            LinkTestInterval = 200,
        });

        var logger = new S9F1WatchLogger();
        await using var connection = new HsmsConnection(opts, logger)
        {
            LinkTestEnabled = false,
        };
        using var secsGem = new SecsGem(opts, connection, logger);

        connection.Start(ct);
        await WaitSelectedAsync(connection, ct).ConfigureAwait(false);

        try
        {
            // SecsGem 内部在收到 DeviceId 不匹配的 data message 后会自动发送 S9F1；
            // 这里通过 logger 观察到 S9F1 出站即可退出。
            await logger.WaitSentAsync(ct).ConfigureAwait(false);
            return 0;
        }
        catch (OperationCanceledException)
        {
            Console.Error.WriteLine("等待 S9F1 出站超时（可能未收到 DeviceId 不匹配的消息）");
            return 4;
        }
    }

    private static async Task<int> TcpRequestClientAsync(string ip, int port, ushort deviceId, CancellationToken ct)
    {
        var opts = Options.Create(new SecsGemOptions
        {
            DeviceId = deviceId,
            IsActive = true,
            IpAddress = ip,
            Port = port,
            T3 = 5000,
            T5 = 2000,
            T6 = 5000,
            T7 = 5000,
            T8 = 2000,
            LinkTestInterval = 200,
        });

        var logger = new SilentLogger();
        await using var connection = new HsmsConnection(opts, logger)
        {
            LinkTestEnabled = true,
        };
        using var secsGem = new SecsGem(opts, connection, logger);

        connection.Start(ct);
        await WaitSelectedAsync(connection, ct).ConfigureAwait(false);

        // 给 LINKTEST 一个机会跑通（如果对端不回，secs4net 会转入 Retry/重连）。
        await Task.Delay(TimeSpan.FromMilliseconds(300), ct).ConfigureAwait(false);
        if (connection.State != ConnectionState.Selected)
        {
            Console.Error.WriteLine($"LINKTEST 后连接未保持 Selected，当前 state={connection.State}");
            return 2;
        }

        var msg = new SecsMessage(1, 13, replyExpected: true)
        {
            SecsItem = MakeTestItem(),
        };
        var rsp = await secsGem.SendAsync(msg, ct).ConfigureAwait(false);
        if (rsp.S != 1 || rsp.F != 14 || rsp.ReplyExpected)
        {
            Console.Error.WriteLine($"响应 S/F/W 校验失败: {rsp}");
            return 3;
        }
        if (rsp.SecsItem is null || !EncodeItem(rsp.SecsItem).AsSpan().SequenceEqual(EncodeItem(msg.SecsItem)))
        {
            Console.Error.WriteLine("响应 Item 与请求不一致");
            return 4;
        }

        return 0;
    }

    private static async Task<int> PipeEchoServerAsync(ushort deviceId, CancellationToken ct)
    {
        var input = Console.OpenStandardInput();
        var output = Console.OpenStandardOutput();

        var selectReq = await ReadFrameAsync(input, ct);
        if (selectReq.SessionId != ControlSessionId || selectReq.SType != MessageType.SelectRequest)
        {
            Console.Error.WriteLine("未收到 SELECT.req");
            return 2;
        }

        var selectRsp = new HsmsFrame(
            ControlSessionId, 0, 0, 0, MessageType.SelectResponse, selectReq.SystemBytes, Array.Empty<byte>());
        await WriteFrameAsync(output, selectRsp, ct);

        // 允许 SELECT 后出现 LINKTEST（C++ 侧会主动发一次 LINKTEST 覆盖互通）。
        HsmsFrame dataReq;
        for (;;)
        {
            var frame = await ReadFrameAsync(input, ct);
            if (frame.SessionId == ControlSessionId && frame.SType == MessageType.LinkTestRequest)
            {
                var ltRsp = new HsmsFrame(
                    ControlSessionId, 0, 0, 0, MessageType.LinkTestResponse, frame.SystemBytes, Array.Empty<byte>());
                await WriteFrameAsync(output, ltRsp, ct);
                continue;
            }
            dataReq = frame;
            break;
        }
        if (dataReq.SessionId != deviceId || dataReq.SType != MessageType.DataMessage)
        {
            Console.Error.WriteLine("未收到 data message");
            return 3;
        }

        var (s, f, wBit) = ParseDataHeader(dataReq.HeaderByte2, dataReq.HeaderByte3);
        if (!wBit)
        {
            Console.Error.WriteLine("主消息 W-bit=0，不符合预期");
            return 4;
        }

        // 只做最小验证：Item 可解码（验证双方 SECS-II 编码互通）
        if (dataReq.Body.Length > 0)
        {
            var seq = new ReadOnlySequence<byte>(dataReq.Body);
            _ = Item.DecodeFromFullBuffer(ref seq);
        }

        var rsp = new HsmsFrame(
            deviceId,
            (byte)(s & 0x7F),
            (byte)(f + 1),
            0,
            MessageType.DataMessage,
            dataReq.SystemBytes,
            dataReq.Body);
        await WriteFrameAsync(output, rsp, ct);

        return 0;
    }

    private static async Task<int> PipeRequestClientAsync(ushort deviceId, CancellationToken ct)
    {
        var input = Console.OpenStandardInput();
        var output = Console.OpenStandardOutput();

        var selectSb = 0x01020304u;
        var selectReq = new HsmsFrame(
            ControlSessionId, 0, 0, 0, MessageType.SelectRequest, selectSb, Array.Empty<byte>());
        await WriteFrameAsync(output, selectReq, ct);

        var selectRsp = await ReadFrameAsync(input, ct);
        if (selectRsp.SessionId != ControlSessionId ||
            selectRsp.SType != MessageType.SelectResponse ||
            selectRsp.SystemBytes != selectSb)
        {
            Console.Error.WriteLine("SELECT.rsp 校验失败");
            return 2;
        }

        var ltSb = 0x05060708u;
        var ltReq = new HsmsFrame(
            ControlSessionId, 0, 0, 0, MessageType.LinkTestRequest, ltSb, Array.Empty<byte>());
        await WriteFrameAsync(output, ltReq, ct);

        var ltRsp = await ReadFrameAsync(input, ct);
        if (ltRsp.SessionId != ControlSessionId ||
            ltRsp.SType != MessageType.LinkTestResponse ||
            ltRsp.SystemBytes != ltSb)
        {
            Console.Error.WriteLine("LINKTEST.rsp 校验失败");
            return 3;
        }

        var itemBytes = EncodeItem(MakeTestItem());
        var sb = 0x0A0B0C0Du;
        var dataReq = new HsmsFrame(
            deviceId,
            (byte)(1 | 0b1000_0000), // S=1, W=1
            13,
            0,
            MessageType.DataMessage,
            sb,
            itemBytes);
        await WriteFrameAsync(output, dataReq, ct);

        var dataRsp = await ReadFrameAsync(input, ct);
        if (dataRsp.SessionId != deviceId ||
            dataRsp.SType != MessageType.DataMessage ||
            dataRsp.SystemBytes != sb)
        {
            Console.Error.WriteLine("响应头校验失败");
            return 4;
        }

        var (s, f, wBit) = ParseDataHeader(dataRsp.HeaderByte2, dataRsp.HeaderByte3);
        if (s != 1 || f != 14 || wBit)
        {
            Console.Error.WriteLine("响应 S/F/W 校验失败");
            return 5;
        }
        if (!dataRsp.Body.AsSpan().SequenceEqual(itemBytes))
        {
            Console.Error.WriteLine("响应 body 与请求不一致");
            return 6;
        }

        return 0;
    }

    private static string GetArg(IReadOnlyList<string> args, string key)
    {
        for (var i = 0; i < args.Count; i++)
        {
            if (args[i] == key && i + 1 < args.Count)
            {
                return args[i + 1];
            }
        }
        throw new ArgumentException($"缺少参数: {key}");
    }

    private static string GetArgOrDefault(IReadOnlyList<string> args, string key, string defaultValue)
    {
        for (var i = 0; i < args.Count; i++)
        {
            if (args[i] == key && i + 1 < args.Count)
            {
                return args[i + 1];
            }
        }
        return defaultValue;
    }

    public static async Task<int> Main(string[] args)
    {
        try
        {
            var list = args.ToList();
            var mode = GetArg(list, "--mode");
            var deviceId = ushort.Parse(GetArg(list, "--device-id"));
            var ip = GetArgOrDefault(list, "--ip", DefaultIp);
            var port = int.TryParse(GetArgOrDefault(list, "--port", "0"), out var p) ? p : 0;

            using var cts = new CancellationTokenSource(TimeSpan.FromSeconds(20));

            return mode switch
            {
                "pipe-echo-server" => await PipeEchoServerAsync(deviceId, cts.Token),
                "pipe-request-client" => await PipeRequestClientAsync(deviceId, cts.Token),
                // 为兼容旧脚本：echo-server / request-client 默认视为 TCP 模式。
                "tcp-echo-server" or "echo-server" => port > 0
                    ? await TcpEchoServerAsync(ip, port, deviceId, cts.Token)
                    : throw new ArgumentException("tcp-echo-server 需要 --port"),
                "tcp-reorder-echo-server" => port > 0
                    ? await TcpReorderEchoServerAsync(ip, port, deviceId, cts.Token)
                    : throw new ArgumentException("tcp-reorder-echo-server 需要 --port"),
                "tcp-s9f1-server" => port > 0
                    ? await TcpS9F1ServerAsync(ip, port, deviceId, cts.Token)
                    : throw new ArgumentException("tcp-s9f1-server 需要 --port"),
                "tcp-request-client" or "request-client" => port > 0
                    ? await TcpRequestClientAsync(ip, port, deviceId, cts.Token)
                    : throw new ArgumentException("tcp-request-client 需要 --port"),
                _ => throw new ArgumentException("未知 mode（支持：pipe-echo-server / pipe-request-client / tcp-echo-server / tcp-reorder-echo-server / tcp-s9f1-server / tcp-request-client）"),
            };
        }
        catch (Exception ex)
        {
            Console.Error.WriteLine(ex);
            return 1;
        }
    }
}
