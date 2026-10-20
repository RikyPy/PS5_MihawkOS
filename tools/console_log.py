# SPDX-License-Identifier: GPL-3.0-or-later
"""Line-oriented capture keeps split UTF-8 and addresses intact before redaction."""
import re


def redact(text):
    return re.sub(r'\b\d{1,3}(?:\.\d{1,3}){3}\b', '<address>', str(text))


class LineLog:
    def __init__(self, output, on_line=None):
        self.output = output
        self.on_line = on_line
        self.pending = b''

    def write(self, chunk):
        self.pending += chunk
        lines = self.pending.split(b'\n')
        self.pending = lines.pop()
        for line in lines:
            text = redact(line.decode('utf-8', 'replace'))
            self.output.write(text + '\n')
            if self.on_line:
                self.on_line(text)
        self.output.flush()

    def finish(self):
        if self.pending:
            self.output.write(redact(self.pending.decode('utf-8', 'replace')))
            self.pending = b''
        self.output.flush()
