#!/usr/bin/env python3
"""Binary contract between BDMV Author (local/SSH) and hdmaenc >=0.0.50."""
import pathlib, subprocess, sys, tempfile, shutil, struct

def main(exe):
    sample=bytearray()
    # Deterministic 48 kHz 16-bit stereo data, sufficient to create 3+ frames.
    for i in range(19200):
        sample += struct.pack('<hh', ((i*37)%1500)-750, ((i*53)%1500)-750)
    args=[exe,'-','--raw-pcm','--sample-rate','48000','--channels','2',
          '--bit-depth','16','--channel-layout','2.0','--lossless',
          '--bitrate','1536000','--xll-decorrelation','on','--no-progress']
    with tempfile.TemporaryDirectory(prefix='bdmv-hdma-') as d:
        path=pathlib.Path(d)/'out.dca'
        seek=subprocess.run(args+['-o',str(path)],input=sample,capture_output=True,check=True)
        stream=subprocess.run(args+['-o','-'],input=sample,capture_output=True,check=True)
        assert seek.stdout.startswith(b'Wrote ') and not stream.stdout.startswith(b'Wrote ')
        assert path.read_bytes()==stream.stdout, 'remote stdout differs from local raw file'
        assert b'Wrote -:' in stream.stderr
        assert stream.stdout[:4]==bytes.fromhex('7ffe8001'), 'missing DCA core sync'
        ffprobe=shutil.which('ffprobe')
        if ffprobe:
            check=subprocess.run([ffprobe,'-v','error','-show_entries',
                'stream=codec_name,profile','-of','default=nw=1',str(path)],
                capture_output=True,text=True,check=True)
            assert 'dts' in check.stdout and 'DTS-HD MA' in check.stdout, check.stdout
    print('hdmaenc raw local/pipe byte parity + FFprobe compatibility PASSED')

if __name__=='__main__':
    main(sys.argv[1])
