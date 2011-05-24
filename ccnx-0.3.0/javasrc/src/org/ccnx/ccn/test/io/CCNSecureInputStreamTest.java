/*
 * A CCNx library test.
 *
 * Copyright (C) 2008, 2009 Palo Alto Research Center, Inc.
 *
 * This work is free software; you can redistribute it and/or modify it under
 * the terms of the GNU General Public License version 2 as published by the
 * Free Software Foundation. 
 * This work is distributed in the hope that it will be useful, but WITHOUT ANY
 * WARRANTY; without even the implied warranty of MERCHANTABILITY or
 * FITNESS FOR A PARTICULAR PURPOSE. See the GNU General Public License
 * for more details. You should have received a copy of the GNU General Public
 * License along with this program; if not, write to the
 * Free Software Foundation, Inc., 51 Franklin Street, Fifth Floor,
 * Boston, MA 02110-1301, USA.
 */

package org.ccnx.ccn.test.io;

import java.io.ByteArrayInputStream;
import java.io.ByteArrayOutputStream;
import java.io.IOException;
import java.io.InputStream;
import java.io.OutputStream;
import java.security.InvalidAlgorithmParameterException;
import java.security.InvalidKeyException;
import java.security.NoSuchAlgorithmException;
import java.security.PrivateKey;
import java.util.Random;
import java.util.logging.Level;

import javax.crypto.BadPaddingException;
import javax.crypto.Cipher;
import javax.crypto.IllegalBlockSizeException;
import javax.crypto.NoSuchPaddingException;

import org.ccnx.ccn.CCNHandle;
import org.ccnx.ccn.impl.security.crypto.ContentKeys;
import org.ccnx.ccn.impl.security.crypto.StaticContentKeys;
import org.ccnx.ccn.impl.security.crypto.UnbufferedCipherInputStream;
import org.ccnx.ccn.impl.support.Log;
import org.ccnx.ccn.io.CCNFileInputStream;
import org.ccnx.ccn.io.CCNFileOutputStream;
import org.ccnx.ccn.io.CCNInputStream;
import org.ccnx.ccn.io.CCNOutputStream;
import org.ccnx.ccn.io.CCNVersionedInputStream;
import org.ccnx.ccn.io.CCNVersionedOutputStream;
import org.ccnx.ccn.io.content.ContentEncodingException;
import org.ccnx.ccn.profiles.SegmentationProfile;
import org.ccnx.ccn.protocol.ContentName;
import org.ccnx.ccn.protocol.ContentObject;
import org.ccnx.ccn.protocol.PublisherPublicKeyDigest;
import org.ccnx.ccn.protocol.SignedInfo;
import org.ccnx.ccn.protocol.SignedInfo.ContentType;
import org.ccnx.ccn.test.CCNTestHelper;
import org.ccnx.ccn.test.Flosser;
import org.junit.AfterClass;
import org.junit.Assert;
import org.junit.BeforeClass;
import org.junit.Test;


/**
 * Test for stream encryption/decryption.
 */
public class CCNSecureInputStreamTest {
	
	static protected abstract class StreamFactory {
		ContentName name;
		ContentKeys keys;
		int encrLength = 25*1024+301;
		byte [] encrData;
		
		public StreamFactory(String file_name) throws NoSuchAlgorithmException, IOException, InterruptedException {
			name = ContentName.fromNative(testHelper.getClassNamespace(), file_name);
			flosser.handleNamespace(name);
			try {
				keys = StaticContentKeys.generateRandomKeys();
			} catch (NoSuchPaddingException e) {
				Log.severe("NoSuchPaddingExcption creating algorithm we have used before! {0}", e.getMessage());
				return;
			}
			writeFile(encrLength);
			flosser.stopMonitoringNamespace(name);
		}
		
		public abstract CCNInputStream makeInputStream() throws IOException;
		public abstract OutputStream makeOutputStream() throws IOException;
		
		public void writeFile(int fileLength) throws IOException, NoSuchAlgorithmException, InterruptedException {
			Random randBytes = new Random(0); // always same sequence, to aid debugging
			OutputStream os = makeOutputStream();

			ByteArrayOutputStream data = new ByteArrayOutputStream();

			byte [] bytes = new byte[BUF_SIZE];
			int elapsed = 0;
			int nextBufSize = 0;
			final double probFlush = .3;

			while (elapsed < fileLength) {
				nextBufSize = ((fileLength - elapsed) > BUF_SIZE) ? BUF_SIZE : (fileLength - elapsed);
				randBytes.nextBytes(bytes);
				os.write(bytes, 0, nextBufSize);
				data.write(bytes, 0, nextBufSize);
				elapsed += nextBufSize;
				if (randBytes.nextDouble() < probFlush) {
					System.out.println("Flushing buffers.");
					os.flush();
				}
			}
			os.close();
			encrData = data.toByteArray();
		}

		public void streamEncryptDecrypt() throws IOException {
			// check we get identical data back out
			CCNInputStream vfirst = makeInputStream();
			byte [] read_data = readFile(vfirst, encrLength);
			Assert.assertArrayEquals(encrData, read_data);

			// check things fail if we use different keys
			ContentKeys keys2 = keys;
			CCNInputStream v2 = null;
			try {
				keys = StaticContentKeys.generateRandomKeys();
				v2 = makeInputStream();
			} catch (NoSuchAlgorithmException e) {
				Log.severe("Unexpected NoSuchAlgorithmException using default algorithm! " + keys.getBaseAlgorithm());
				Assert.fail("Unexpected NoSuchAlgorithmException using default algorithm! " + keys.getBaseAlgorithm());
			} catch (NoSuchPaddingException e) {
				Log.severe("Unexpected NoSuchPaddingException using default algorithm! " + keys.getBaseAlgorithm());
				Assert.fail("Unexpected NoSuchPaddingException using default algorithm! " + keys.getBaseAlgorithm());
			} finally {
				keys = keys2;
			}
			read_data = readFile(v2, encrLength);
			Assert.assertFalse(encrData.equals(read_data));
		}

		public void seeking() throws IOException, NoSuchAlgorithmException {
			// check really small seeks/reads (smaller than 1 Cipher block)
			doSeeking(10);

			// check small seeks (but bigger than 1 Cipher block)
			doSeeking(600);

			// check large seeks (multiple ContentObjects)
			doSeeking(4096*5+350);
		}

		private void doSeeking(int length) throws IOException, NoSuchAlgorithmException {
			CCNInputStream i = makeInputStream();
			// make sure we start mid ContentObject and past the first Cipher block
			int start = ((int) (encrLength*0.3) % 4096) +600;
			i.seek(start);
			readAndCheck(i, start, length);
			i.seek(start);
			readAndCheck(i, start, length);
		}

		public void markReset() throws IOException, NoSuchAlgorithmException {
			// check really small seeks/reads (smaller than 1 Cipher block)
			doMarkReset(10);

			// check small seeks (but bigger than 1 Cipher block)
			doMarkReset(600);

			// check large seeks (multiple ContentObjects)
			doMarkReset(4096*2+350);
		}

		private void doMarkReset(int length) throws IOException, NoSuchAlgorithmException {
			CCNInputStream i = makeInputStream();
			i.skip(length);
			i.reset();
			readAndCheck(i, 0, length);
			i.skip(1024);
			i.mark(length);
			readAndCheck(i, length+1024, length);
			i.reset();
			readAndCheck(i, length+1024, length);
		}

		private void readAndCheck(CCNInputStream i, int start, int length)
				throws IOException, NoSuchAlgorithmException {
			byte [] origData = new byte[length];
			System.arraycopy(encrData, start, origData, 0, length);
			byte [] readData = new byte[length];
			i.read(readData);
			Assert.assertArrayEquals(origData, readData);
		}

		public void skipping() throws IOException, NoSuchAlgorithmException {
			// read some data, skip some data, read some more data
			CCNInputStream inStream = makeInputStream();

			int start = (int) (encrLength*0.3);

			// check first part reads correctly
			readAndCheck(inStream, 0, start);

			// skip a short bit (less than 1 cipher block)
			inStream.skip(10);
			start += 10;

			// check second part reads correctly
			readAndCheck(inStream, start, 100);
			start += 100;

			// skip a medium bit (more than than 1 cipher block)
			inStream.skip(600);
			start += 600;

			// check third part reads correctly
			readAndCheck(inStream, start, 600);
			start += 600;

			// skip a bug bit (more than than 1 Content object)
			inStream.skip(600+4096*2);
			start += 600+4096*2;

			// check fourth part reads correctly
			readAndCheck(inStream, start, 600);
		}
	}

	/**
	 * Handle naming for the test
	 */
	static CCNTestHelper testHelper = new CCNTestHelper(CCNSecureInputStreamTest.class);
	
	static CCNHandle outputLibrary;
	static CCNHandle inputLibrary;
	static Flosser flosser;
	static final int BUF_SIZE = 4096;

	static StreamFactory basic;
	static StreamFactory versioned;
	static StreamFactory file;

	@BeforeClass
	public static void setUpBeforeClass() throws Exception {
		Log.setDefaultLevel(Level.FINEST);
		Log.setDefaultLevel(Log.FAC_SIGNING, Level.FINEST);
		outputLibrary = CCNHandle.open();
		inputLibrary = CCNHandle.open();
				
		flosser = new Flosser();
		
		basic = new StreamFactory("basic.txt"){
			public CCNInputStream makeInputStream() throws IOException {
				return new CCNInputStream(name, null, null, keys, inputLibrary);
			}
			public OutputStream makeOutputStream() throws IOException {
				return new CCNOutputStream(name, null, null, null, keys, outputLibrary);
			}
		};

		versioned = new StreamFactory("versioned.txt"){
			public CCNInputStream makeInputStream() throws IOException {
				return new CCNVersionedInputStream(name, 0L, null, keys, inputLibrary);
			}
			public OutputStream makeOutputStream() throws IOException {
				return new CCNVersionedOutputStream(name, null, null, keys, outputLibrary);
			}
		};

		file = new StreamFactory("file.txt"){
			public CCNInputStream makeInputStream() throws IOException {
				return new CCNFileInputStream(name, null, null, keys, inputLibrary);
			}
			public OutputStream makeOutputStream() throws IOException {
				return new CCNFileOutputStream(name, keys, outputLibrary);
			}
		};
		flosser.stop();
	}
	
	@AfterClass
	public static void cleanupAfterClass() {
		outputLibrary.close();
		inputLibrary.close();
	}
	
	public static byte [] readFile(InputStream inputStream, int fileLength) throws IOException {
		ByteArrayOutputStream bos = null;
		bos = new ByteArrayOutputStream();
		int elapsed = 0;
		int read = 0;
		byte [] bytes = new byte[BUF_SIZE];
		while (elapsed < fileLength) {
			read = inputStream.read(bytes);
			bos.write(bytes, 0, read);
			if (read < 0) {
				break;
			} else if (read == 0) {
				try {
					Thread.sleep(10);
				} catch (InterruptedException e) {
					
				}
			}
			elapsed += read;
		}
		return bos.toByteArray();
	}
	
	/**
	 * Test cipher encryption & decryption work
	 * @throws ContentEncodingException 
	 */
	@Test
	public void cipherEncryptDecrypt() throws InvalidKeyException, InvalidAlgorithmParameterException, IllegalBlockSizeException, BadPaddingException, ContentEncodingException {
		Cipher c = basic.keys.getSegmentEncryptionCipher(basic.name, outputLibrary.getDefaultPublisher(), 0);
		byte [] d = c.doFinal(basic.encrData);
		c = basic.keys.getSegmentDecryptionCipher(basic.name, outputLibrary.getDefaultPublisher(), 0);
		d = c.doFinal(d);
		// check we get identical data back out
		Assert.assertArrayEquals(basic.encrData, d);
	}

	/**
	 * Test cipher stream encryption & decryption work
	 * @throws IOException
	 */
	@Test
	public void cipherStreamEncryptDecrypt() throws InvalidKeyException, InvalidAlgorithmParameterException, IllegalBlockSizeException, BadPaddingException, IOException {
		Cipher c = basic.keys.getSegmentEncryptionCipher(basic.name, outputLibrary.getDefaultPublisher(),0);
		InputStream is = new ByteArrayInputStream(basic.encrData, 0, basic.encrData.length);
		is = new UnbufferedCipherInputStream(is, c);
		byte [] cipherText = new byte[4096];
		for(int total = 0, res = 0; res >= 0 && total < 4096; total+=res)
			res = is.read(cipherText,total,4096-total);

		c = basic.keys.getSegmentDecryptionCipher(basic.name, outputLibrary.getDefaultPublisher(), 0);
		is = new ByteArrayInputStream(cipherText);
		is = new UnbufferedCipherInputStream(is, c);
		byte [] output = new byte[4096];
		for(int total = 0, res = 0; res >= 0 && total < 4096; total+=res)
			res = is.read(output,total,4096-total);
		// check we get identical data back out
		byte [] input = new byte[Math.min(4096, basic.encrLength)];
		System.arraycopy(basic.encrData, 0, input, 0, input.length);
		Assert.assertArrayEquals(input, output);
	}

	/**
	 * Test content encryption & decryption work
	 * @throws IOException
	 */
	@Test
	public void contentEncryptDecrypt() throws InvalidKeyException, InvalidAlgorithmParameterException, IllegalBlockSizeException, BadPaddingException, IOException {
		// create an encrypted content block
		PublisherPublicKeyDigest publisher = outputLibrary.getDefaultPublisher();
		Cipher c = basic.keys.getSegmentEncryptionCipher(basic.name, publisher, 0);
		InputStream is = new ByteArrayInputStream(basic.encrData, 0, basic.encrData.length);
		is = new UnbufferedCipherInputStream(is, c);
		ContentName rootName = SegmentationProfile.segmentRoot(basic.name);
		PrivateKey signingKey = outputLibrary.keyManager().getSigningKey(publisher);
		byte [] finalBlockID = SegmentationProfile.getSegmentNumberNameComponent(1);
		ContentObject co = new ContentObject(SegmentationProfile.segmentName(rootName, 0),
				new SignedInfo(publisher, null, ContentType.ENCR, outputLibrary.keyManager().getKeyLocator(signingKey), new Integer(300), finalBlockID),
				is, 4096);

		// attempt to decrypt the data
		c = basic.keys.getSegmentDecryptionCipher(basic.name, publisher, 0);
		is = new UnbufferedCipherInputStream(new ByteArrayInputStream(co.content()), c);
		byte [] output = new byte[co.contentLength()];
		for(int total = 0, res = 0; res >= 0 && total < output.length; total+=res)
			res = is.read(output, total, output.length-total);
		// check we get identical data back out
		byte [] input = new byte[Math.min(4096, co.contentLength())];
		System.arraycopy(basic.encrData, 0, input, 0, input.length);
		Assert.assertArrayEquals(input, output);
	}

	/**
	 * Test stream encryption & decryption work, and that using different keys for decryption fails
	 */
	@Test
	public void basicStreamEncryptDecrypt() throws IOException {
		basic.streamEncryptDecrypt();
	}
	@Test
	public void versionedStreamEncryptDecrypt() throws IOException {
		versioned.streamEncryptDecrypt();
	}
	@Test
	public void fileStreamEncryptDecrypt() throws IOException {
		file.streamEncryptDecrypt();
	}

	/**
	 * seek forward, read, seek back, read and check the results
	 * do it for different size parts of the data
	 */
	@Test
	public void basicSeeking() throws IOException, NoSuchAlgorithmException {
		basic.seeking();
	}
	@Test
	public void versionedSeeking() throws IOException, NoSuchAlgorithmException {
		versioned.seeking();
	}
	@Test
	public void fileSeeking() throws IOException, NoSuchAlgorithmException {
		file.seeking();
	}

	/**
	 * Test that skipping while reading an encrypted stream works
	 * Tries small/medium/large skips
	 */
	@Test
	public void basicSkipping() throws IOException, NoSuchAlgorithmException {
		basic.skipping();
	}
	@Test
	public void versionedSkipping() throws IOException, NoSuchAlgorithmException {
		versioned.skipping();
	}
	@Test
	public void fileSkipping() throws IOException, NoSuchAlgorithmException {
		file.skipping();
	}

	/**
	 * Test that mark and reset on an encrypted stream works
	 * Tries small/medium/large jumps
	 */
	@Test
	public void basicMarkReset() throws IOException, NoSuchAlgorithmException {
		basic.markReset();
	}
	@Test
	public void versionedMarkReset() throws IOException, NoSuchAlgorithmException {
		versioned.markReset();
	}
	@Test
	public void fileMarkReset() throws IOException, NoSuchAlgorithmException {
		file.markReset();
	}
}
