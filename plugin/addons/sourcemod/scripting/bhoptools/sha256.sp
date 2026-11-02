int Sha256Rotr(int x, int n)
{
	return (x >>> n) | (x << (32 - n));
}

void Sha256Block(int h[8], const int block[64])
{
	static const int k[64] =
	{
		0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5,
		0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3, 0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174,
		0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
		0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967,
		0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85,
		0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
		0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3,
		0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208, 0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2
	};

	int w[64];

	for (int i = 0; i < 16; i++)
	{
		w[i] = (block[i * 4] << 24) | (block[i * 4 + 1] << 16) | (block[i * 4 + 2] << 8) | block[i * 4 + 3];
	}

	for (int i = 16; i < 64; i++)
	{
		int s0 = Sha256Rotr(w[i - 15], 7) ^ Sha256Rotr(w[i - 15], 18) ^ (w[i - 15] >>> 3);
		int s1 = Sha256Rotr(w[i - 2], 17) ^ Sha256Rotr(w[i - 2], 19) ^ (w[i - 2] >>> 10);
		w[i] = w[i - 16] + s0 + w[i - 7] + s1;
	}

	int a = h[0];
	int b = h[1];
	int c = h[2];
	int d = h[3];
	int e = h[4];
	int f = h[5];
	int g = h[6];
	int hh = h[7];

	for (int i = 0; i < 64; i++)
	{
		int s1 = Sha256Rotr(e, 6) ^ Sha256Rotr(e, 11) ^ Sha256Rotr(e, 25);
		int ch = (e & f) ^ (~e & g);
		int t1 = hh + s1 + ch + k[i] + w[i];
		int s0 = Sha256Rotr(a, 2) ^ Sha256Rotr(a, 13) ^ Sha256Rotr(a, 22);
		int maj = (a & b) ^ (a & c) ^ (b & c);
		int t2 = s0 + maj;
		hh = g;
		g = f;
		f = e;
		e = d + t1;
		d = c;
		c = b;
		b = a;
		a = t1 + t2;
	}

	h[0] += a;
	h[1] += b;
	h[2] += c;
	h[3] += d;
	h[4] += e;
	h[5] += f;
	h[6] += g;
	h[7] += hh;
}

bool Sha256File(const char[] path, char[] output, int maxlength)
{
	File file = OpenFile(path, "rb");

	if (file == null)
	{
		return false;
	}

	int h[8] = {0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a, 0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19};
	int block[64];
	int total = 0;
	bool done = false;

	while (!done)
	{
		int n = file.Read(block, 64, 1);

		if (n < 0)
		{
			delete file;
			return false;
		}

		total += n;

		if (n == 64)
		{
			Sha256Block(h, block);
			continue;
		}

		block[n++] = 0x80;

		if (n > 56)
		{
			while (n < 64)
			{
				block[n++] = 0;
			}

			Sha256Block(h, block);
			n = 0;
		}

		while (n < 56)
		{
			block[n++] = 0;
		}

		int bits = total * 8;
		block[56] = 0;
		block[57] = 0;
		block[58] = 0;
		block[59] = 0;
		block[60] = (bits >>> 24) & 0xff;
		block[61] = (bits >>> 16) & 0xff;
		block[62] = (bits >>> 8) & 0xff;
		block[63] = bits & 0xff;
		Sha256Block(h, block);
		done = true;
	}

	delete file;
	Format(output, maxlength, "%08x%08x%08x%08x%08x%08x%08x%08x", h[0], h[1], h[2], h[3], h[4], h[5], h[6], h[7]);
	return true;
}
