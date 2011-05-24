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

package org.ccnx.ccn.test;


import static org.junit.Assert.fail;

import java.io.IOException;
import java.security.InvalidKeyException;
import java.security.SignatureException;
import java.util.ArrayList;
import java.util.Date;
import java.util.HashSet;
import java.util.Random;
import java.util.concurrent.Semaphore;
import java.util.logging.Level;

import org.ccnx.ccn.CCNFilterListener;
import org.ccnx.ccn.CCNHandle;
import org.ccnx.ccn.CCNInterestListener;
import org.ccnx.ccn.config.ConfigurationException;
import org.ccnx.ccn.config.SystemConfiguration;
import org.ccnx.ccn.impl.CCNFlowControl;
import org.ccnx.ccn.impl.support.Log;
import org.ccnx.ccn.io.CCNWriter;
import org.ccnx.ccn.protocol.ContentName;
import org.ccnx.ccn.protocol.ContentObject;
import org.ccnx.ccn.protocol.Interest;
import org.ccnx.ccn.protocol.MalformedContentNameStringException;
import org.junit.AfterClass;
import org.junit.BeforeClass;


/**
 * 
 * A base class for the old style of library tests. 
 * Defines a few common parameters, and a test-running framework which passes data
 * between different threads or objects, via ccnd.
 * New tests should probably not use this without some additional cleanup.
 *
 */
public class LibraryTestBase extends CCNTestBase {

	protected static boolean exit = false;
	protected static Throwable error = null; // for errors from other threads
	public static int count = 55;
	//public static int count = 5;
	public static Random rand = new Random();
	public static final int WAIT_DELAY = 200000;
	
	protected static final String BASE_NAME = "/test/BaseLibraryTest/";
	protected static ContentName PARENT_NAME;
	
	protected static final boolean DO_TAP = true;
		
	protected HashSet<Integer> _resultSet = new HashSet<Integer>();
	
	protected static ArrayList<Integer> usedIds = new ArrayList<Integer>();
	
	static {
		try {
			PARENT_NAME = ContentName.fromNative(BASE_NAME);
		} catch (MalformedContentNameStringException e) {}
	}

	@BeforeClass
	public static void setUpBeforeClass() throws Exception {
		CCNTestBase.setUpBeforeClass();
		// Let default logging level be set centrally so it can be overridden by property
	}
	
	@AfterClass
	public static void tearDownAfterClass() throws Exception {
		CCNTestBase.tearDownAfterClass();
	}

	public void genericGetPut(Thread putter, Thread getter) throws Throwable {
		try {
			putter.start();
			Thread.sleep(20);
			Date start = new Date();
			getter.start();
			putter.join(WAIT_DELAY);
			getter.join(WAIT_DELAY);
			boolean good = true;
			exit = true;
			if (getter.getState() != Thread.State.TERMINATED) {
				getter.interrupt();
				System.out.println("Get Thread has not finished!");
				good = false;
			}
			if (putter.getState() != Thread.State.TERMINATED) {
				putter.interrupt();
				System.out.println("Put Thread has not finished!");
				good = false;
			}
			if (null != error) {
				System.out.println("Error in test thread: " + error.getClass().toString());
				throw error;
			}
			if (!good) {
				fail();
			}
			System.out.println("Get/Put test in " + (new Date().getTime() - start.getTime()) + " ms");
		} catch (InterruptedException e) {
			e.printStackTrace();
			fail("InterruptedException");
		}
	}
	
	/**
	 * Subclassible object processing operations, to make it possible to easily
	 * implement tests based on this one.
	 * @author smetters
	 *
	 */
	public void checkGetResults(ContentObject getResults) {
		System.out.println("Got result: " + getResults.name());
	}
	
	public void checkPutResults(ContentName putResult) {
		System.out.println("Put data: " + putResult);
	}
	
	/**
	 * Expects this method to call checkGetResults on each set of content returned...
	 * @param baseName
	 * @param count
	 * @param handle
	 * @return
	 * @throws InterruptedException
	 * @throws IOException
	 * @throws SignatureException 
	 * @throws InvalidKeyException 
	 * @throws InterruptedException 
	 */
	public void getResults(ContentName baseName, int count, CCNHandle handle) throws IOException, InvalidKeyException, SignatureException, InterruptedException {
		Random rand = new Random();
	//	boolean done = false;
		System.out.println("getResults: getting children of " + baseName);
		for (int i = 0; i < count; i++) {
	//	while (!done) {
			Thread.sleep(rand.nextInt(50));
			System.out.println("getResults getting " + baseName + " subitem " + i);
			ContentObject contents = handle.get(ContentName.fromNative(baseName, Integer.toString(i)), SystemConfiguration.NO_TIMEOUT);
		
			try {
				int val = Integer.parseInt(new String(contents.content()));
				if (_resultSet.contains(val)) {
					System.out.println("Got " + val + " again.");
				} else {
					System.out.println("Got " + val);
				}
				_resultSet.add(val);

			} catch (NumberFormatException nfe) {
				Log.info("BaseLibraryTest: unexpected content - not integer. Name: " + contents.content());
			}
			//assertEquals(i, Integer.parseInt(new String(contents.get(0).content())));
			checkGetResults(contents);
			
			if (_resultSet.size() == count) {
				System.out.println("We have everything!");
//				done = true; 
			}
		}
		return;
	}
	
	/**
	 * Responsible for calling checkPutResults on each put. (Could return them all in
	 * a batch then check...)
	 * @throws InterruptedException 
	 * @throws IOException 
	 * @throws MalformedContentNameStringException 
	 * @throws SignatureException 
	 * @throws InvalidKeyException 
	 */
	public void doPuts(ContentName baseName, int count, CCNHandle handle) 
			throws InterruptedException, SignatureException, MalformedContentNameStringException, IOException, InvalidKeyException {

		CCNWriter writer = new CCNWriter(baseName, handle);
		Random rand = new Random();
		for (int i = 0; i < count; i++) {
			Thread.sleep(rand.nextInt(50));
			ContentName putResult = writer.put(ContentName.fromNative(baseName, Integer.toString(i)), new Integer(i).toString().getBytes());
			System.out.println("Put " + i + " done");
			checkPutResults(putResult);
		}
		writer.close();
	}
	
	public int getUniqueId() {
		int id;
		do {
			id = rand.nextInt(1000);
		} while (usedIds.indexOf(id) != -1);
		usedIds.add(id);
		return id;
	}
	
	public class GetThread implements Runnable {
		protected CCNHandle handle = null;
		int count = 0;
		int id = 0;
		public GetThread(int n, int id) throws ConfigurationException, IOException {
			handle = CCNHandle.open();
			count = n;
			this.id = id;
			if (DO_TAP) {
				try {
					((CCNHandle)handle).getNetworkManager().setTap(SystemConfiguration.DEBUG_DATA_DIRECTORY + "/LibraryTestDebug_" + Integer.toString(id) + "_get");
				} catch (IOException ie) {
				}
			}
		}
		public void run() {
			try {
				System.out.println("Get thread started");
				getResults(ContentName.fromNative(PARENT_NAME, Integer.toString(id)), count, handle);
				handle.close();
				System.out.println("Get thread finished");
			} catch (Throwable ex) {
				error = ex;
				Log.warning("Exception in run: " + ex.getClass().getName() + " message: " + ex.getMessage());
				Log.logStackTrace(Level.WARNING, ex);
			}
		}
	}
	
	public class PutThread implements Runnable {
		protected CCNHandle handle = null;
		int count = 0;
		int id = 0;
		public PutThread(int n, int id) throws ConfigurationException, IOException {
			handle = CCNHandle.open();
			count = n;
			this.id = id;
			if (DO_TAP) {
				try {
					((CCNHandle)handle).getNetworkManager().setTap(SystemConfiguration.DEBUG_DATA_DIRECTORY + "/LibraryTestDebug_" + Integer.toString(id) + "_put");
				} catch (IOException ie) {
				}
			}
		}
		public void run() {
			try {
				System.out.println("Put thread started");
				doPuts(ContentName.fromNative(PARENT_NAME, Integer.toString(id)), count, handle);
				handle.close();
				System.out.println("Put thread finished");
				//cf.shutdown();
			} catch (Throwable ex) {
				error = ex;
				Log.warning("Exception in run: " + ex.getClass().getName() + " message: " + ex.getMessage());
				Log.logStackTrace(Level.WARNING, ex);
			}
		}
	}
	
	public class GetServer implements Runnable, CCNInterestListener {
		protected CCNHandle handle = null;
		int count = 0;
		int next = 0;
		Semaphore sema = new Semaphore(0);
		HashSet<Integer> accumulatedResults = new HashSet<Integer>();
		int id;
		
		public GetServer(int n, int id) throws ConfigurationException, IOException {
			handle = CCNHandle.open();
			count = n;
			this.id = id;
			if (DO_TAP) {
				try {
					((CCNHandle)handle).getNetworkManager().setTap(SystemConfiguration.DEBUG_DATA_DIRECTORY + "/LibraryTestDebug_" + Integer.toString(id) + "_get");
				} catch (IOException ie) {
				}
			}
		}
		public void run() {
			try {
				System.out.println("GetServer started");
				Interest interest = new Interest(ContentName.fromNative(PARENT_NAME, Integer.toString(id)));
				// Register interest
				handle.expressInterest(interest, this);
				// Block on semaphore until enough data has been received
				boolean interrupted = false;
				do {
					try {
						sema.acquire();
					} catch (InterruptedException ie) { interrupted = true; }
				} while (interrupted);
				handle.cancelInterest(interest, this);
				handle.close();

			} catch (Throwable ex) {
				error = ex;
				Log.warning("Exception in run: " + ex.getClass().getName() + " message: " + ex.getMessage());
				Log.logStackTrace(Level.WARNING, ex);
			}
		}
		public synchronized Interest handleContent(ContentObject contentObject, Interest interest) {
			Interest newInterest = null;
			try {
				int val = Integer.parseInt(new String(contentObject.content()));
				if (!accumulatedResults.contains(val)) {
					accumulatedResults.add(val);
					System.out.println("Got " + val);
				}
				newInterest = Interest.next(contentObject.fullName(), contentObject.name().count() - 2, null);
			} catch (NumberFormatException nfe) {
				Log.info("Unexpected content, " + contentObject.name() + " is not an integer!");
			}
			checkGetResults(contentObject);

			if (accumulatedResults.size() >= count) {
				System.out.println("GetServer got all content: " + accumulatedResults.size() + ". Releasing semaphore.");
				sema.release();
			}
			return  newInterest;
		}
	}
	
	public class PutServer implements Runnable, CCNFilterListener {
		protected CCNHandle handle = null;
		int count = 0;
		int next = 0;
		Semaphore sema = new Semaphore(0);
		ContentName name = null;
		HashSet<Integer> accumulatedResults = new HashSet<Integer>();
		int id;
		CCNFlowControl cf = null;
		CCNWriter writer = null;
		
		public PutServer(int n, int id) throws ConfigurationException, IOException {
			handle = CCNHandle.open();
			count = n;
			this.id = id;
			if (DO_TAP) {
				try {
					((CCNHandle)handle).getNetworkManager().setTap(SystemConfiguration.DEBUG_DATA_DIRECTORY + "/LibraryTestDebug_" + Integer.toString(id) + "_put");
				} catch (IOException ie) {
				}
			}
		}
		
		public void run() {
			try {
				System.out.println("PutServer started");
				// Register filter
				name = ContentName.fromNative(PARENT_NAME, Integer.toString(id));
				writer = new CCNWriter(name, handle);
				handle.registerFilter(name, this);
				// Block on semaphore until enough data has been received
				sema.acquire();
				handle.unregisterFilter(name, this);
				System.out.println("PutServer finished.");
				handle.close();

			} catch (Throwable ex) {
				error = ex;
				Log.warning("Exception in run: " + ex.getClass().getName() + " message: " + ex.getMessage());
				Log.logStackTrace(Level.WARNING, ex);
			}
		}

		public synchronized boolean handleInterest(Interest interest) {
			boolean result = false;
			try {
				try {
					int val = Integer.parseInt(new String(interest.name().component(interest.name().count()-1)));
					System.out.println("Got interest in " + val);
					if (!accumulatedResults.contains(val)) {
						ContentName putResult = writer.put(ContentName.fromNative(name, Integer.toString(val)), Integer.toString(next).getBytes());
						result = true;
						System.out.println("Put " + val + " done");
						checkPutResults(putResult);
						next++;
						accumulatedResults.add(val);
					}
				} catch (NumberFormatException nfe) {
					Log.info("Unexpected interest, " + interest.name() + " does not end in an integer!");
				}
				if (accumulatedResults.size() >= count) {
					sema.release();
				}
			} catch (Throwable e) {
				error = e;
				Log.warning("Exception in run: " + e.getClass().getName() + " message: " + e.getMessage());
				Log.logStackTrace(Level.WARNING, e);
			}
			return result;
		}
	}
}
