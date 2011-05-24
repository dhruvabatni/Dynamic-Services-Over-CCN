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

package org.ccnx.ccn.test.io.content;

import java.io.ByteArrayOutputStream;
import java.io.IOException;
import java.io.InvalidObjectException;

import org.bouncycastle.util.Arrays;
import org.ccnx.ccn.CCNHandle;
import org.ccnx.ccn.impl.CCNFlowControl.SaveType;
import org.ccnx.ccn.impl.security.crypto.util.DigestHelper;
import org.ccnx.ccn.impl.support.Log;
import org.ccnx.ccn.io.CCNVersionedInputStream;
import org.ccnx.ccn.io.content.CCNNetworkObject;
import org.ccnx.ccn.io.content.CCNStringObject;
import org.ccnx.ccn.io.content.Collection;
import org.ccnx.ccn.io.content.Link;
import org.ccnx.ccn.io.content.LinkAuthenticator;
import org.ccnx.ccn.io.content.LocalCopyListener;
import org.ccnx.ccn.io.content.LocalCopyWrapper;
import org.ccnx.ccn.io.content.UpdateListener;
import org.ccnx.ccn.io.content.Collection.CollectionObject;
import org.ccnx.ccn.profiles.VersioningProfile;
import org.ccnx.ccn.profiles.repo.RepositoryControl;
import org.ccnx.ccn.protocol.CCNTime;
import org.ccnx.ccn.protocol.ContentName;
import org.ccnx.ccn.protocol.PublisherID;
import org.ccnx.ccn.protocol.SignedInfo;
import org.ccnx.ccn.protocol.PublisherID.PublisherType;
import org.ccnx.ccn.test.CCNTestHelper;
import org.ccnx.ccn.test.io.content.CCNNetworkObjectTest.CounterListener;
import org.junit.AfterClass;
import org.junit.Assert;
import org.junit.BeforeClass;
import org.junit.Test;


/**
 * Test basic network object functionality, writing objects to a repository.
 **/
public class CCNNetworkObjectTestRepo {
	
	/**
	 * Handle naming for the test
	 */
	static CCNTestHelper testHelper = new CCNTestHelper(CCNNetworkObjectTestRepo.class);

	static String stringObjName = "StringObject";
	static String collectionObjName = "CollectionObject";
	static String prefix = "CollectionObject-";
	static ContentName [] ns = null;
	
	static public byte [] contenthash1 = new byte[32];
	static public byte [] contenthash2 = new byte[32];
	static public byte [] publisherid1 = new byte[32];
	static public byte [] publisherid2 = new byte[32];
	static PublisherID pubID1 = null;	
	static PublisherID pubID2 = null;
	static int NUM_LINKS = 15;
	static LinkAuthenticator [] las = new LinkAuthenticator[NUM_LINKS];
	static Link [] lrs = null;
	
	static Collection small1;
	static Collection small2;
	static Collection empty;
	static Collection big;
	static CCNHandle handle;
	static String [] numbers = new String[]{"ONE", "TWO", "THREE", "FOUR", "FIVE", "SIX", "SEVEN", "EIGHT", "NINE", "TEN"};
	
	static void setupNamespace(ContentName name) throws IOException {
	}
	
	static void removeNamepspace(ContentName name) throws IOException {
	}
	
	@AfterClass
	public static void tearDownAfterClass() throws Exception {
		Log.info("Tearing down CCNNetworkObjectTestRepo, prefix {0}", testHelper.getClassNamespace());
	}

	@BeforeClass
	public static void setUpBeforeClass() throws Exception {
		Log.info("Setting up CCNNetworkObjectTestRepo, prefix {0}", testHelper.getClassNamespace());
		
		handle = CCNHandle.open();
		
		ns = new ContentName[NUM_LINKS];
		for (int i=0; i < NUM_LINKS; ++i) {
			ns[i] = ContentName.fromNative(testHelper.getClassNamespace(), "Links", prefix+Integer.toString(i));
		}
		Arrays.fill(publisherid1, (byte)6);
		Arrays.fill(publisherid2, (byte)3);

		pubID1 = new PublisherID(publisherid1, PublisherType.KEY);
		pubID2 = new PublisherID(publisherid2, PublisherType.ISSUER_KEY);

		las[0] = new LinkAuthenticator(pubID1);
		las[1] = null;
		las[2] = new LinkAuthenticator(pubID2, null, null,
				SignedInfo.ContentType.DATA, contenthash1);
		las[3] = new LinkAuthenticator(pubID1, null, CCNTime.now(),
				null, contenthash1);
		
		for (int j=4; j < NUM_LINKS; ++j) {
			las[j] = new LinkAuthenticator(pubID2, null, CCNTime.now(),null, null);
 		}

		lrs = new Link[NUM_LINKS];
		for (int i=0; i < lrs.length; ++i) {
			lrs[i] = new Link(ns[i],las[i]);
		}
		
		empty = new Collection();
		small1 = new Collection();
		small2 = new Collection();
		for (int i=0; i < 5; ++i) {
			small1.add(lrs[i]);
			small2.add(lrs[i+5]);
		}
		big = new Collection();
		for (int i=0; i < NUM_LINKS; ++i) {
			big.add(lrs[i]);
		}
		Log.info("Finihed setting up CCNNetworkObjectTestRepo, prefix {0}", testHelper.getClassNamespace());
		handle.close();
	}

	@Test
	public void testVersioning() throws Exception {
		// Testing problem of disappearing versions, inability to get latest. Use simpler
		// object than a collection.
		CCNHandle lput = CCNHandle.open();
		CCNHandle lget = CCNHandle.open();
		
		ContentName testName = ContentName.fromNative(testHelper.getTestNamespace("testVersioning"), stringObjName);
		
		CCNStringObject so = new CCNStringObject(testName, "First value", SaveType.REPOSITORY, lput);
		setupNamespace(testName);
		CCNStringObject ro = null;
		CCNStringObject ro2 = null;
		CCNStringObject ro3, ro4; // make each time, to get a new handle.
		CCNTime soTime, srTime, sr2Time, sr3Time, sr4Time, so2Time;
		for (int i=0; i < numbers.length; ++i) {
			soTime = saveAndLog(numbers[i], so, null, numbers[i]);
			if (null == ro) {
				ro = new CCNStringObject(testName, lget);
				srTime = waitForDataAndLog(numbers[i], ro);
			} else {
				srTime = updateAndLog(numbers[i], ro, null);				
			}
			if (null == ro2) {
				ro2 = new CCNStringObject(testName, null);
				sr2Time = waitForDataAndLog(numbers[i], ro2);
			} else {
				sr2Time = updateAndLog(numbers[i], ro2, null);				
			}
			ro3 = new CCNStringObject(ro.getVersionedName(), null); // read specific version
			sr3Time = waitForDataAndLog("UpdateToROVersion", ro3);
			// Save a new version and pull old
			so2Time = saveAndLog(numbers[i] + "-Update", so, null, numbers[i] + "-Update");
			ro4 = new CCNStringObject(ro.getVersionedName(), null); // read specific version
			sr4Time = waitForDataAndLog("UpdateAnotherToROVersion", ro4);
			System.out.println("Update " + i + ": Times: " + soTime + " " + srTime + " " + sr2Time + " " + sr3Time + " different: " + so2Time);
			Assert.assertEquals("SaveTime doesn't match first read", soTime, srTime);
			Assert.assertEquals("SaveTime doesn't match second read", soTime, sr2Time);
			Assert.assertEquals("SaveTime doesn't match specific version read", soTime, sr3Time);
			Assert.assertFalse("UpdateTime isn't newer than read time", soTime.equals(so2Time));
			Assert.assertEquals("SaveTime doesn't match specific version read", soTime, sr4Time);
		}
		lput.close();
		lget.close();
	}

	@Test
	public void testSaveToVersion() throws Exception {
		// Testing problem of disappearing versions, inability to get latest. Use simpler
		// object than a collection.
		CCNHandle lput = CCNHandle.open();
		CCNHandle lget = CCNHandle.open();
		ContentName testName = ContentName.fromNative(testHelper.getTestNamespace("testSaveToVersion"), stringObjName);
		
		CCNTime desiredVersion = CCNTime.now();
		
		CCNStringObject so = new CCNStringObject(testName, "First value", SaveType.REPOSITORY, lput);
		setupNamespace(testName);
		saveAndLog("SpecifiedVersion", so, desiredVersion, "Time: " + desiredVersion);
		Assert.assertEquals("Didn't write correct version", desiredVersion, so.getVersion());
		
		CCNStringObject ro = new CCNStringObject(testName, lget);
		ro.waitForData(); 
		Assert.assertEquals("Didn't read correct version", desiredVersion, ro.getVersion());
		ContentName versionName = ro.getVersionedName();
		
		saveAndLog("UpdatedVersion", so, null, "ReplacementData");
		updateAndLog("UpdatedData", ro, null);
		Assert.assertTrue("New version " + so.getVersion() + " should be later than old version " + desiredVersion, (desiredVersion.before(so.getVersion())));
		Assert.assertEquals("Didn't read correct version", so.getVersion(), ro.getVersion());
		
		CCNStringObject ro2 = new CCNStringObject(versionName, null);
		ro2.waitForData();
		Assert.assertEquals("Didn't read correct version", desiredVersion, ro2.getVersion());
		lput.close();
		lget.close();
	}

	@Test
	public void testEmptySave() throws Exception {
		boolean caught = false;
		ContentName testName = ContentName.fromNative(testHelper.getTestNamespace("testEmptySave"), collectionObjName);
		CollectionObject emptycoll = 
			new CollectionObject(testName, (Collection)null, SaveType.REPOSITORY, handle);
		setupNamespace(testName);
		try {
			emptycoll.setData(small1); // set temporarily to non-null
			saveAndLog("Empty", emptycoll, null, null);
		} catch (InvalidObjectException iox) {
			// this is what we expect to happen
			caught = true;
		}
		Assert.assertTrue("Failed to produce expected exception.", caught);		
	}
	
	@Test
	public void testStreamUpdate() throws Exception {
		
		CCNHandle tHandle = CCNHandle.open();
		ContentName testName = ContentName.fromNative(testHelper.getTestNamespace("testStreamUpdate"), collectionObjName);
		CollectionObject testCollectionObject = 
			new CollectionObject(testName, small1, SaveType.REPOSITORY, tHandle);
		setupNamespace(testName);
		
		saveAndLog("testStreamUpdate", testCollectionObject, null, small1);
		System.out.println("testCollectionObject name: " + testCollectionObject.getVersionedName());
				
		CCNVersionedInputStream vis = new CCNVersionedInputStream(testCollectionObject.getVersionedName());
		ByteArrayOutputStream baos = new ByteArrayOutputStream();
		byte [] buf = new byte[128];
		// Will incur a timeout
		while (!vis.eof()) {
			int read = vis.read(buf);
			if (read > 0)
				baos.write(buf, 0, read);
		}
		System.out.println("Read " + baos.toByteArray().length + " bytes, digest: " + 
				DigestHelper.printBytes(DigestHelper.digest(baos.toByteArray()), 16));

		Collection decodedData = new Collection();
		decodedData.decode(baos.toByteArray());
		System.out.println("Decoded collection data: " + decodedData);
		Assert.assertEquals("Decoding via stream fails to give expected result!", decodedData, small1);
		
		CCNVersionedInputStream vis2 = new CCNVersionedInputStream(testCollectionObject.getVersionedName());
		ByteArrayOutputStream baos2 = new ByteArrayOutputStream();
		// Will incur a timeout
		while (!vis2.eof()) {
			int val = vis2.read();
			if (val < 0)
				break;
			baos2.write((byte)val);
		}
		System.out.println("Read " + baos2.toByteArray().length + " bytes, digest: " + 
				DigestHelper.printBytes(DigestHelper.digest(baos2.toByteArray()), 16));
		Assert.assertArrayEquals("Reading same object twice gets different results!", baos.toByteArray(), baos2.toByteArray());

		Collection decodedData2 = new Collection();
		decodedData2.decode(baos2.toByteArray());
		Assert.assertEquals("Decoding via stream byte read fails to give expected result!", decodedData2, small1);

		CCNVersionedInputStream vis3 = new CCNVersionedInputStream(testCollectionObject.getVersionedName());
		Collection decodedData3 = new Collection();
		decodedData3.decode(vis3);
		Assert.assertEquals("Decoding via stream full read fails to give expected result!", decodedData3, small1);
		tHandle.close();
	}
	
	@Test
	public void testVersionOrdering() throws Exception {
		CCNHandle tHandle = CCNHandle.open();
		ContentName testName = ContentName.fromNative(testHelper.getTestNamespace("testVersionOrdering"), collectionObjName, "name1");
		ContentName testName2 = ContentName.fromNative(testHelper.getTestNamespace("testVersionOrdering"), collectionObjName, "name2");
		
		CollectionObject c0 = new CollectionObject(testName, empty, SaveType.REPOSITORY, handle);
		setupNamespace(testName);
		CCNTime t0 = saveAndLog("Empty", c0, null, empty);
		
		CollectionObject c1 = new CollectionObject(testName2, small1, SaveType.REPOSITORY, tHandle);
		CollectionObject c2 = new CollectionObject(testName2, small1, SaveType.REPOSITORY, null);
		setupNamespace(testName2);
		CCNTime t1 = saveAndLog("Small", c1, null, small1);
		Assert.assertTrue("First version should come before second", t0.before(t1));
		
		CCNTime t2 = saveAndLog("Small2ndWrite", c2, null, small1);
		Assert.assertTrue("Third version should come after second", t1.before(t2));
		Assert.assertTrue(c2.contentEquals(c1));
		Assert.assertFalse(c2.equals(c1));
		Assert.assertTrue(VersioningProfile.isLaterVersionOf(c2.getVersionedName(), c1.getVersionedName()));
		tHandle.close();
	}
	
	@Test
	public void testUpdateInBackground() throws Exception {
		
		CCNHandle thandle = CCNHandle.open();
		CCNHandle thandle2 = CCNHandle.open();
		CCNHandle thandle3 = CCNHandle.open();
		ContentName testName = ContentName.fromNative(testHelper.getTestNamespace("testUpdateInBackground"), stringObjName, "name1");
		// Make writer first, so it picks up readers first interests even before it writes.
		CCNStringObject c2 = new CCNStringObject(testName, (String)null, SaveType.REPOSITORY, thandle);
		
		CCNStringObject c0 = new CCNStringObject(testName, (String)null, SaveType.REPOSITORY, thandle2);
		c0.updateInBackground();

		CCNStringObject c1 = new CCNStringObject(testName, (String)null, SaveType.REPOSITORY, thandle3);
		c1.updateInBackground(true);

		Assert.assertFalse(c0.available());
		Assert.assertFalse(c0.isSaved());
		Assert.assertFalse(c1.available());
		Assert.assertFalse(c1.isSaved());

		CCNTime t1 = saveAndLog("First string", c2, null, "Here is the first string.");
		System.out.println("Saved c2: " + c2.getVersionedName() + " c0 available? " + c0.available() + " c1 available? " + c1.available());
		c0.waitForData();
		Assert.assertEquals("c0 update", c0.getVersion(), c2.getVersion());
		c1.waitForData();
		Assert.assertEquals("c1 update", c1.getVersion(), c2.getVersion());

		CCNTime t2 = saveAndLog("Second string", c2, null, "Here is the second string.");
		if (!c1.getVersion().equals(t2)) {
			synchronized (c1) {
				c1.wait(5000);
			}
		}
		Assert.assertEquals("c1 update 2", c1.getVersion(), c2.getVersion());
		Assert.assertEquals("c0 unchanged", c0.getVersion(), t1);
		
		// Sleep for a while and see how fast the interests come. Should be only 2 interests.
		System.out.println("Sleeping, count background interests.");
		long time = System.currentTimeMillis();
		Thread.sleep(2000);
		long elapsed = System.currentTimeMillis() - time;
		long count = (elapsed/4000) + 1;
		System.out.println("Slept " + elapsed/1000.0 + " seconds, should have been " + count + " interests.");
		
		CCNTime t3 = saveAndLog("Third string", c2, null, "Here is the third string.");
		if (!c1.getVersion().equals(t3)) {
			synchronized (c1) {
				c1.wait(5000);
			}
		}
		Assert.assertEquals("c1 update 3", c1.getVersion(), c2.getVersion());
		Assert.assertEquals("c0 unchanged", c0.getVersion(), t1);
		
		c1.cancelInterest();
		thandle.close();
		thandle2.close();
		thandle3.close();
	}
	
	@Test
	public void testUpdateOtherName() throws Exception {
		CCNHandle thandle = CCNHandle.open();
		ContentName testName = ContentName.fromNative(testHelper.getTestNamespace("testUpdateOtherName"), collectionObjName, "name1");
		ContentName testName2 = ContentName.fromNative(testHelper.getTestNamespace("testUpdateOtherName"), collectionObjName, "name2");

		CollectionObject c0 = new CollectionObject(testName, empty, SaveType.REPOSITORY, handle);
		setupNamespace(testName);
		CCNTime t0 = saveAndLog("Empty", c0, null, empty);
		
		CollectionObject c1 = new CollectionObject(testName2, small1, SaveType.REPOSITORY, thandle);
		// Cheat a little, make this one before the setupNamespace...
		CollectionObject c2 = new CollectionObject(testName2, small1, SaveType.REPOSITORY, null);
		setupNamespace(testName2);
		CCNTime t1 = saveAndLog("Small", c1, null, small1);
		Assert.assertTrue("First version should come before second", t0.before(t1));
		
		CCNTime t2 = saveAndLog("Small2ndWrite", c2, null, small1);
		Assert.assertTrue("Third version should come after second", t1.before(t2));
		Assert.assertTrue(c2.contentEquals(c1));
		Assert.assertFalse(c2.equals(c1));
		
		CCNTime t3 = updateAndLog(c0.getVersionedName().toString(), c0, testName2);
		Assert.assertTrue(VersioningProfile.isVersionOf(c0.getVersionedName(), testName2));
		Assert.assertEquals(t3, t2);
		Assert.assertTrue(c0.contentEquals(c2));
		
		t3 = updateAndLog(c0.getVersionedName().toString(), c0, c1.getVersionedName());
		Assert.assertTrue(VersioningProfile.isVersionOf(c0.getVersionedName(), testName2));
		Assert.assertEquals(t3, t1);
		Assert.assertTrue(c0.contentEquals(c1));	
		thandle.close();
	}

	
	@Test
	public void testSaveAsGone() throws Exception {
		CCNHandle thandle = CCNHandle.open();
		ContentName testName = ContentName.fromNative(testHelper.getTestNamespace("testSaveAsGone"), collectionObjName);
		CollectionObject c0 = new CollectionObject(testName, empty, SaveType.REPOSITORY, handle);
		setupNamespace(testName);
		CCNTime t0 = saveAsGoneAndLog("Gone", c0);
		Assert.assertTrue("Should be gone", c0.isGone());
		ContentName goneVersionName = c0.getVersionedName();
		
		CCNTime t1 = saveAndLog("NotGone", c0, null, small1);
		Assert.assertFalse("Should not be gone", c0.isGone());
		Assert.assertTrue(t1.after(t0));
		
		CollectionObject c1 = new CollectionObject(testName, thandle);
		CCNTime t2 = waitForDataAndLog(testName.toString(), c1);
		Assert.assertFalse("Read back should not be gone", c1.isGone());
		Assert.assertEquals(t2, t1);
		
		CCNTime t3 = updateAndLog(goneVersionName.toString(), c1, goneVersionName);
		Assert.assertTrue(VersioningProfile.isVersionOf(c1.getVersionedName(), testName));
		Assert.assertEquals(t3, t0);
		Assert.assertTrue("Read back should be gone.", c1.isGone());

		t0 = saveAsGoneAndLog("GoneAgain", c0);
		Assert.assertTrue("Should be gone", c0.isGone());
		
		CollectionObject c2 = new CollectionObject(testName, CCNHandle.open());
		CCNTime t4 = waitForDataAndLog(testName.toString(), c2);
		Assert.assertTrue("Read back of " + c0.getVersionedName() + " should be gone, got " + c2.getVersionedName(), c2.isGone());
		Assert.assertEquals(t4, t0);
		thandle.close();

	}
	
	@Test
	public void testUpdateDoesNotExist() throws Exception {
		CCNHandle thandle = CCNHandle.open();
		ContentName testName = ContentName.fromNative(testHelper.getTestNamespace("testUpdateDoesNotExist"), collectionObjName);
		CCNStringObject so = new CCNStringObject(testName, handle);
		// so should catch exception thrown by underlying stream when it times out.
		Assert.assertFalse(so.available());
		
		// ask for it in background
		so.updateInBackground();
		
		CCNStringObject sowrite = new CCNStringObject(testName, "Now we write something.", SaveType.REPOSITORY, thandle);
		setupNamespace(testName);
		saveAndLog("Delayed write", sowrite, null, "Now we write something.");
		so.waitForData();
		Assert.assertTrue(so.available());
		Assert.assertEquals(so.string(), sowrite.string());
		Assert.assertEquals(so.getVersionedName(), sowrite.getVersionedName());
		thandle.close();
	}
	
	@Test
	public void testUpdateListener() throws Exception {
		
		SaveType saveType = SaveType.REPOSITORY;
		CCNHandle writeHandle = CCNHandle.open();
		CCNHandle readHandle = CCNHandle.open();
		ContentName testName = ContentName.fromNative(testHelper.getTestNamespace("testUpdateListener"), 
										stringObjName);
		
		CCNStringObject writeObject = 
			new CCNStringObject(testName, "Something to listen to.", saveType, writeHandle);
		writeObject.save();
		
		CounterListener ourListener = new CounterListener();
		CCNStringObject readObject = 
			new CCNStringObject(testName, null, null, readHandle);
		readObject.addListener(ourListener);
		boolean result = readObject.update();
		
		Assert.assertTrue(result);
		Assert.assertTrue(ourListener.getCounter() == 1);
		
		readObject.updateInBackground();
		
		writeObject.save("New stuff! New stuff!");
		synchronized(readObject) {
			if (ourListener.getCounter() == 1)
				readObject.wait();
		}
		// For some reason, we're getting two updates on our updateInBackground...
		Assert.assertTrue(ourListener.getCounter() > 1);
		writeHandle.close();
		readHandle.close();
	}

	@Test
	public void testLocalCopyWrapper() throws Exception {
		CCNHandle thandle = CCNHandle.open();
		ContentName testName = ContentName.fromNative(testHelper.getTestNamespace("testLocalCopyWrapper"), collectionObjName);
		CCNStringObject so = new CCNStringObject(testName, handle);
		LocalCopyWrapper wo = new LocalCopyWrapper(so);
		Assert.assertFalse(wo.available());
		class Record { boolean callback = false; }
		Record record = new Record();
		
		class Listener implements UpdateListener {
			Record _rec;
			
			public Listener(Record r) {
				_rec = r;
			}
			public void newVersionAvailable(CCNNetworkObject<?> newVersion, boolean wasSave) {
				synchronized (_rec) {
					_rec.callback = true;
					_rec.notifyAll();
				}
			}
		};
		
		// ask for it in background
		wo.updateInBackground(false, new Listener(record));
		
		CCNStringObject sowrite = new CCNStringObject(testName, "Now we write", SaveType.RAW, thandle);
		setupNamespace(testName);
		saveAndLog("Delayed write", sowrite, null, "Now we write");
		wo.waitForData();
		Assert.assertTrue(wo.available());
		Assert.assertEquals(((CCNStringObject)wo.object()).string(), sowrite.string());
		Assert.assertEquals(wo.getVersionedName(), sowrite.getVersionedName());
		
		synchronized (record) {
			if (!record.callback) {
				record.wait(5000);
			}
			Assert.assertEquals(true, record.callback);
		}
		// Should be in the repo by now
		Assert.assertTrue(RepositoryControl.localRepoSync(handle, so));
		thandle.close();
	}
	
	@Test
	public void testLocalCopyListener() throws Exception {
		
		// The local copy wrapper API isn't applicable in very many situations. Try this instead.
		
		ContentName testName = ContentName.fromNative(testHelper.getTestNamespace("testLocalCopyListener"), collectionObjName);
		
		LocalCopyListener copyListener = new LocalCopyListener();
		
		// Would like to test functionality that arranges for existing version to get written
		// to the repo, but can't do that without a flosser or using a flow server.
		CCNStringObject so = new CCNStringObject(testName, handle);
		so.addListener(copyListener);
		
		Assert.assertFalse(so.available());
		
		class Record { boolean callback = false; }
		Record record = new Record();
		
		class Listener implements UpdateListener {
			Record _rec;
			
			public Listener(Record r) {
				_rec = r;
			}
			public void newVersionAvailable(CCNNetworkObject<?> newVersion, boolean wasSave) {
				synchronized (_rec) {
					_rec.callback = true;
					_rec.notifyAll();
				}
			}
		};
		
		// ask for it in background
		so.updateInBackground(false, new Listener(record));
		
		CCNHandle thandle = CCNHandle.open();
		CCNStringObject sowrite = new CCNStringObject(testName, "Now we write", SaveType.RAW, thandle);
		setupNamespace(testName);
		saveAndLog("Delayed write", sowrite, null, "Now we write");
		so.waitForData();
		Assert.assertTrue(so.available());
		Assert.assertEquals(so.string(), sowrite.string());
		Assert.assertEquals(so.getVersionedName(), sowrite.getVersionedName());
		
		synchronized (record) {
			if (!record.callback) {
				record.wait(5000);
			}
			Assert.assertEquals(true, record.callback);
		}
		// Should be in the repo by now
		Assert.assertTrue(RepositoryControl.localRepoSync(handle, so));
		thandle.close();
	}


	public <T> CCNTime saveAndLog(String name, CCNNetworkObject<T> ecd, CCNTime version, T data) throws IOException {
		CCNTime oldVersion = ecd.getVersion();
		ecd.save(version, data);
		Log.info(name + " Saved " + name + ": " + ecd.getVersionedName() + " (" + ecd.getVersion() + ", updated from " + oldVersion + ")" +  " gone? " + ecd.isGone() + " data: " + ecd);
		return ecd.getVersion();
	}
	
	public <T> CCNTime saveAsGoneAndLog(String name, CCNNetworkObject<T> ecd) throws IOException {
		CCNTime oldVersion = ecd.getVersion();
		ecd.saveAsGone();
		Log.info("Saved " + name + ": " + ecd.getVersionedName() + " (" + ecd.getVersion() + ", updated from " + oldVersion + ")" +  " gone? " + ecd.isGone() + " data: " + ecd);
		return ecd.getVersion();
	}
	
	public CCNTime waitForDataAndLog(String name, CCNNetworkObject<?> ecd) throws IOException {
		ecd.waitForData();
		Log.info("Initial read " + name + ", name: " + ecd.getVersionedName() + " (" + ecd.getVersion() +")" +  " gone? " + ecd.isGone() + " data: " + ecd);
		return ecd.getVersion();
	}

	public CCNTime updateAndLog(String name, CCNNetworkObject<?> ecd, ContentName updateName) throws IOException {
		if ((null == updateName) ? ecd.update() : ecd.update(updateName, null))
			Log.info("Updated " + name + ", to name: " + ecd.getVersionedName() + " (" + ecd.getVersion() +")" +  " gone? " + ecd.isGone() + " data: " + ecd);
		else 
			Log.info("No update found for " + name + ((null != updateName) ? (" at name " + updateName) : "") + ", still: " + ecd.getVersionedName() + " (" + ecd.getVersion() +")" +  " gone? " + ecd.isGone() + " data: " + ecd);
		return ecd.getVersion();
	}
}
