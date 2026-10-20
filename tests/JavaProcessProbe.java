/* SPDX-License-Identifier: GPL-3.0-or-later */
import java.io.*;
import java.nio.charset.StandardCharsets;
import java.nio.file.*;
import java.util.*;
import java.util.concurrent.TimeUnit;

public class JavaProcessProbe {
    static final String INPUT = "input with spaces and \u03bb\n";
    static final String COOKIE = "java-child-cookie";
    static void require(boolean ok, String stage) {
        if (!ok) throw new AssertionError(stage);
    }
    static byte[] drain(InputStream in) {
        /* The installed JDK's readAllBytes reports "Invalid function" on
         * the pinned host Wine's pipes; ordinary stream reads work. */
        try {
            ByteArrayOutputStream out = new ByteArrayOutputStream();
            byte[] buffer = new byte[1024];
            for (int count; (count = in.read(buffer)) != -1;) out.write(buffer, 0, count);
            return out.toByteArray();
        }
        catch (IOException e) { throw new UncheckedIOException(e); }
    }
    static void spawn(int depth) throws Exception {
        String java = Path.of(System.getProperty("java.home"), "bin", "java.exe").toString();
        String classes = Path.of(System.getProperty("java.class.path")).toAbsolutePath().toString();
        ProcessBuilder builder = new ProcessBuilder(java, "-Xint", "-Xms16m", "-Xmx64m",
                "-XX:+UseSerialGC", "-XX:ActiveProcessorCount=2", "-Djava.awt.headless=true",
                "-cp", classes, "JavaProcessProbe", "child", "" + depth,
                "" + ProcessHandle.current().pid(), "two words");
        builder.directory(new File(classes));
        builder.environment().put("PS5_JAVA_COOKIE", COOKIE);
        builder.environment().put("WINE_PS5_DESCENDANTS", "1");
        Process child = builder.start();
        long pid = child.pid();
        require(pid != ProcessHandle.current().pid(), "distinct process identity");
        byte[][] output = new byte[2][];
        Thread stdout = new Thread(() -> output[0] = drain(child.getInputStream()));
        Thread stderr = new Thread(() -> output[1] = drain(child.getErrorStream()));
        stdout.start(); stderr.start();
        child.getOutputStream().write(INPUT.getBytes(StandardCharsets.UTF_8));
        child.getOutputStream().close();
        require(child.waitFor(60, TimeUnit.SECONDS), "child deadline");
        stdout.join(5000); stderr.join(5000);
        require(!stdout.isAlive() && !stderr.isAlive() && output[0] != null && output[1] != null, "pipe EOF");
        String out = new String(output[0], StandardCharsets.UTF_8).replace("\r\n", "\n");
        String err = new String(output[1], StandardCharsets.UTF_8).replace("\r\n", "\n");
        require(child.exitValue() == 0x5100 + depth, "full exit code=" + child.exitValue() + " stderr=" + err);
        require(out.contains("JAVA_NODE PASS depth=" + depth + " pid=" + pid + "\n"), "PID and stdout");
        require(out.contains("PIPE=" + "x".repeat(4096) + "\n"), "buffered stdout pipe");
        require(err.contains("JAVA_STDERR depth=" + depth + "\n"), "stderr pipe");
        System.out.write(output[0]); System.err.write(output[1]);
    }
    public static void main(String[] args) throws Exception {
        if (args.length == 0) {
            spawn(2);
            System.out.println("JAVA_PROCESS_ROOT PASS");
            return;
        }
        require(args.length == 4 && args[0].equals("child") && args[3].equals("two words"), "quoted arguments");
        int depth = Integer.parseInt(args[1]);
        long parent = Long.parseLong(args[2]);
        require(ProcessHandle.current().parent().orElseThrow().pid() == parent, "immediate Windows parent");
        require(COOKIE.equals(System.getenv("PS5_JAVA_COOKIE")), "environment");
        require(new File(".").getCanonicalPath().equalsIgnoreCase(new File(System.getProperty("java.class.path")).getCanonicalPath()), "working directory");
        require(new String(drain(System.in), StandardCharsets.UTF_8).equals(INPUT), "stdin pipe");
        System.out.println("JAVA_NODE PASS depth=" + depth + " pid=" + ProcessHandle.current().pid());
        System.out.println("PIPE=" + "x".repeat(4096));
        System.err.println("JAVA_STDERR depth=" + depth);
        if (depth > 0) spawn(depth - 1);
        System.exit(0x5100 + depth);
    }
}
