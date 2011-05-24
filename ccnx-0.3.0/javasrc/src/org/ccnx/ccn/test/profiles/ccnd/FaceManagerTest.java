/*
 * A CCNx library test.
 *
 * Copyright (C) 2009, 2010 Palo Alto Research Center, Inc.
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

package org.ccnx.ccn.test.profiles.ccnd;

import static org.junit.Assert.assertEquals;
import static org.junit.Assert.assertNotNull;
import static org.junit.Assert.fail;

import java.io.ByteArrayInputStream;
import java.io.ByteArrayOutputStream;

import org.ccnx.ccn.impl.CCNNetworkManager;
import org.ccnx.ccn.impl.CCNNetworkManager.NetworkProtocol;
import org.ccnx.ccn.io.content.ContentDecodingException;
import org.ccnx.ccn.io.content.ContentEncodingException;
import org.ccnx.ccn.profiles.ccnd.CCNDaemonException;
import org.ccnx.ccn.profiles.ccnd.FaceManager;
import org.ccnx.ccn.profiles.ccnd.FaceManager.ActionType;
import org.ccnx.ccn.profiles.ccnd.FaceManager.FaceInstance;
import org.ccnx.ccn.protocol.PublisherPublicKeyDigest;
import org.ccnx.ccn.test.LibraryTestBase;
import org.ccnx.ccn.test.impl.encoding.XMLEncodableTester;
import org.junit.After;
import org.junit.AfterClass;
import org.junit.Before;
import org.junit.BeforeClass;
import org.junit.Test;

/**
 * Test basic version manipulation.
 */
public class FaceManagerTest extends LibraryTestBase {
	
	PublisherPublicKeyDigest keyDigest;
	FaceManager fm;


	/**
	 * @throws java.lang.Exception
	 */
	@BeforeClass
	public static void setUpBeforeClass() throws Exception {
	}

	/**
	 * @throws java.lang.Exception
	 */
	@AfterClass
	public static void tearDownAfterClass() throws Exception {
	}

	/**
	 * @throws java.lang.Exception
	 */
	@Before
	public void setUp() throws Exception {
		keyDigest = null; /* new PublisherPublicKeyDigest(); */
		fm = new FaceManager();
	}

	/**
	 * @throws java.lang.Exception
	 */
	@After
	public void tearDown() throws Exception {
	}
	
	/**
	 * Test method for org.ccnx.ccn.profiles.VersioningProfile#addVersion(org.ccnx.ccn.protocol.ContentName, long).
	 */
	@Test
	public void testEncodeOutputStream() {
		FaceInstance face = fm. new FaceInstance(ActionType.NewFace, keyDigest, NetworkProtocol.TCP, "TheNameDoesntMatter", 
				new Integer(5),	"WhoCares", new Integer(42), new Integer(100));
		// ActionType.NewFace, _ccndId, ipProto, host, port,  multicastInterface, multicastTTL, freshnessSeconds
		System.out.println("Encoding: " + face);
		assertNotNull(face);
		
		ByteArrayOutputStream baos = new ByteArrayOutputStream();
		try {
			face.encode(baos);
		} catch (ContentEncodingException e) {
			System.out.println("Exception " + e.getClass().getName() + ", message: " + e.getMessage());
			e.printStackTrace();
		}
		System.out.println("Encoded: " );
		System.out.println(baos.toString());
	}

	@Test
	public void testDecodeInputStream() {
		FaceInstance faceToEncode = fm. new FaceInstance(ActionType.NewFace, keyDigest, NetworkProtocol.TCP, "TheNameDoesntMatter", 
				new Integer(5),	"WhoCares", new Integer(42), new Integer(100));
		System.out.println("Encoding: " + faceToEncode);
		assertNotNull(faceToEncode);
		
		ByteArrayOutputStream baos = new ByteArrayOutputStream();
		try {
			faceToEncode.encode(baos);
		} catch (ContentEncodingException e) {
			System.out.println("Exception " + e.getClass().getName() + ", message: " + e.getMessage());
			e.printStackTrace();
		}
		System.out.println("Encoded: " );
		System.out.println(baos.toString());
		
		System.out.println("Decoding: ");
		ByteArrayInputStream bais = new ByteArrayInputStream(baos.toByteArray());
		FaceInstance faceToDecodeTo = fm. new FaceInstance();  /* We need an empty one to decode into */
		try {
			faceToDecodeTo.decode(bais);
		} catch (ContentDecodingException e) {
			System.out.println("Exception " + e.getClass().getName() + ", message: " + e.getMessage());
			e.printStackTrace();
		}
		System.out.println("Decoded: " + faceToDecodeTo);
		assertEquals(faceToEncode, faceToDecodeTo);
	}
	
	@Test
	public void testEncodingDecoding() {
		FaceInstance faceToEncode = fm. new FaceInstance(ActionType.NewFace, keyDigest, NetworkProtocol.TCP, "TheNameDoesntMatter", 
				new Integer(5),	"WhoCares", new Integer(42), new Integer(100));
		System.out.println("Encoding: " + faceToEncode);

		FaceInstance  textFaceToDecodeInto = fm. new FaceInstance();
		assertNotNull(textFaceToDecodeInto);
		FaceInstance  binaryFaceToDecodeInto = fm. new FaceInstance();
		assertNotNull(binaryFaceToDecodeInto);
		XMLEncodableTester.encodeDecodeTest("FaceIntance", faceToEncode, textFaceToDecodeInto, binaryFaceToDecodeInto);
	}
	
	@Test
	public void testCreation() {
		Integer faceID = new Integer(-142);
		FaceManager mgr = null;
		try {
			mgr = new FaceManager(putHandle);
			faceID = mgr.createFace(NetworkProtocol.UDP, "10.1.1.1", new Integer(CCNNetworkManager.DEFAULT_AGENT_PORT));
			System.out.println("Created face: " + faceID);
		} catch (CCNDaemonException e) {
			System.out.println("Exception " + e.getClass().getName() + ", message: " + e.getMessage());
			System.out.println("Failed to create face.");
			e.printStackTrace();
			fail("Failed to create face.");
		}
		assertNotNull(mgr);
		try {
			mgr.deleteFace(faceID);
		}catch (CCNDaemonException e) {
			System.out.println("Exception " + e.getClass().getName() + ", message: " + e.getMessage());
			System.out.println("Failed to delete face.");
			e.printStackTrace();
			fail("Failed to delete face.");
		}
	}

}
