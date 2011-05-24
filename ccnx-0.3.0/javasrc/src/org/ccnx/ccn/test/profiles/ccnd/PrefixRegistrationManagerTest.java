/*
 * A CCNx library test.
 *
 * Copyright (C) 2008, 2009, 2010 Palo Alto Research Center, Inc.
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

import org.ccnx.ccn.io.content.ContentDecodingException;
import org.ccnx.ccn.io.content.ContentEncodingException;
import org.ccnx.ccn.profiles.ccnd.CCNDaemonException;
import org.ccnx.ccn.profiles.ccnd.PrefixRegistrationManager;
import org.ccnx.ccn.profiles.ccnd.PrefixRegistrationManager.ActionType;
import org.ccnx.ccn.profiles.ccnd.PrefixRegistrationManager.ForwardingEntry;
import org.ccnx.ccn.protocol.ContentName;
import org.ccnx.ccn.protocol.MalformedContentNameStringException;
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
public class PrefixRegistrationManagerTest extends LibraryTestBase {
	
	PublisherPublicKeyDigest keyDigest;
	PrefixRegistrationManager prm;
	ContentName contentNameToUse;
	public final static String prefixToUse = "ccnx:/prefix/to/test/with/";


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
		prm = new PrefixRegistrationManager();
		contentNameToUse = ContentName.fromURI(prefixToUse);
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
		System.out.println();
		System.out.println("PrefixRegistrationManagerTest.testEncodeOutputStream:");
		ForwardingEntry entryToEncode = prm. new ForwardingEntry(ActionType.Register, contentNameToUse, keyDigest, new Integer(42), new Integer(3), new Integer(149));
		System.out.println("Encoding: " + entryToEncode);
		assertNotNull("EncodeOutputStream", entryToEncode);
		
		ByteArrayOutputStream baos = new ByteArrayOutputStream();
		try {
			entryToEncode.encode(baos);
		} catch (ContentEncodingException e) {
			System.out.println("Exception " + e.getClass().getName() + ", message: " + e.getMessage());
			e.printStackTrace();
		}
		System.out.println("Encoded: " );
		System.out.println(ContentName.componentPrintURI(baos.toString().getBytes()));
		System.out.println();
	}

	@Test
	public void testDecodeInputStream() {
		System.out.println();
		System.out.println("PrefixRegistrationManagerTest.testDecodeInputStream:");
		ForwardingEntry entryToEncode = prm. new ForwardingEntry(ActionType.Register, contentNameToUse, keyDigest, new Integer(42), new Integer(3), new Integer(149));
		System.out.println("Encoding: " + entryToEncode);
		assertNotNull("DecodeOutputStream", entryToEncode);
		
		ByteArrayOutputStream baos = new ByteArrayOutputStream();
		try {
			entryToEncode.encode(baos);
		} catch (ContentEncodingException e) {
			System.out.println("Exception " + e.getClass().getName() + ", message: " + e.getMessage());
			e.printStackTrace();
		}
		System.out.println("Encoded: " );
		System.out.println(ContentName.componentPrintURI(baos.toString().getBytes()));
		
		System.out.println("Decoding: ");
		ByteArrayInputStream bais = new ByteArrayInputStream(baos.toByteArray());
		ForwardingEntry entryToDecodeTo = prm. new ForwardingEntry();  /* We need an empty one to decode into */
		try {
			entryToDecodeTo.decode(bais);
		} catch (ContentDecodingException e) {
			System.out.println("Exception " + e.getClass().getName() + ", message: " + e.getMessage());
			e.printStackTrace();
		}
		System.out.println("Decoded: " + entryToDecodeTo);
		assertEquals("DecodeOutputStream", entryToEncode, entryToDecodeTo);
		System.out.println();
	}
	
	@Test
	public void testEncodingDecoding() {
		System.out.println();
		System.out.println("PrefixRegistrationManagerTest.testEncodingDecoding:");
		ForwardingEntry entryToEncode = prm. new ForwardingEntry(ActionType.Register, contentNameToUse, keyDigest, new Integer(42), new Integer(3), new Integer(149));
		System.out.println("Encoding: " + entryToEncode);

		ForwardingEntry  textEntryToDecodeInto = prm. new ForwardingEntry();
		assertNotNull("EncodeDecodeOutput", textEntryToDecodeInto);
		ForwardingEntry  binaryEntryToDecodeInto = prm. new ForwardingEntry();
		assertNotNull("EncodeDecodeOutput", binaryEntryToDecodeInto);
		XMLEncodableTester.encodeDecodeTest("EncodeDecodeOutput", entryToEncode, textEntryToDecodeInto, binaryEntryToDecodeInto);
		System.out.println();
	}

	
	@Test
	public void testCreation() {
		System.out.println();
		System.out.println("PrefixRegistrationManagerTest.testCreation:");
		Integer faceID = null;
		ContentName testCN = null;
		PrefixRegistrationManager manager = null;
		try {
			testCN = ContentName.fromURI(prefixToUse);
		} catch (MalformedContentNameStringException e1) {
			e1.printStackTrace();
			fail("ContentName.fromURI(prefixToUse) Failed.  Reason: " + e1.getMessage());
		}
		
		try {
			manager = new PrefixRegistrationManager(putHandle);
			ForwardingEntry entry = manager.selfRegisterPrefix(testCN);
			faceID = entry.getFaceID();
			System.out.println("Created prefix: " + testCN + " on face " + faceID + " with lifetime " + entry.getLifetime());
		} catch (CCNDaemonException e) {
			System.out.println("Exception " + e.getClass().getName() + ", message: " + e.getMessage());
			System.out.println("Failed to self register prefix.");
			e.printStackTrace();
			fail("Failed to self register prefix.  Reason: " + e.getMessage());
		}
		assertNotNull(prm);
		try {
			manager.unRegisterPrefix(testCN, faceID);
		}catch (CCNDaemonException e) {
			System.out.println("Exception " + e.getClass().getName() + ", message: " + e.getMessage());
			System.out.println("Failed to delete prefix.");
			e.printStackTrace();
			fail("Failed to delete prefix.  Reason: " + e.getMessage());
		}
		System.out.println();
		
		try {
			manager.registerPrefix(testCN, faceID, null);
			System.out.println("Created prefix: " + testCN + " on face " + faceID);
		} catch (CCNDaemonException e) {
			System.out.println("Exception " + e.getClass().getName() + ", message: " + e.getMessage());
			System.out.println("Failed to self register prefix.");
			e.printStackTrace();
			fail("Failed to self register prefix.  Reason: " + e.getMessage());
		}
		assertNotNull(prm);
		try {
			manager.unRegisterPrefix(testCN, faceID);
		}catch (CCNDaemonException e) {
			System.out.println("Exception " + e.getClass().getName() + ", message: " + e.getMessage());
			System.out.println("Failed to delete prefix.");
			e.printStackTrace();
			fail("Failed to delete prefix.  Reason: " + e.getMessage());
		}
	}

}
