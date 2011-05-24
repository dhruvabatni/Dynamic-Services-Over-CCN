/*
 * CCNx Android Services
 *
 * Copyright (C) 2010 Palo Alto Research Center, Inc.
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

package org.ccnx.android.services;

import org.ccnx.android.ccnlib.CCNxServiceControl;
import org.ccnx.android.ccnlib.CCNxServiceCallback;
import org.ccnx.android.ccnlib.CCNxServiceStatus.SERVICE_STATUS;

import android.app.Activity;
import android.app.ProgressDialog;
import android.content.Context;
import android.os.Bundle;
import android.os.Handler;
import android.os.Message;
import android.util.Log;
import android.view.View;
import android.view.View.OnClickListener;
import android.widget.Button;
import android.widget.TextView;

/**
 * Android UI for controlling CCNx services.
 */
public final class Controller extends Activity implements OnClickListener {
	public final static String TAG = "CCNx Service Controller";
	
	Button allBtn;
	
	ProgressDialog pd;
	
	Context _ctx;
	
	TextView tvCcndStatus;
	TextView tvRepoStatus;
	
	CCNxServiceControl control;
	
	// Create a handler to receive status updates
	private final Handler _handler = new Handler() {
		public void handleMessage(Message msg){
			SERVICE_STATUS st = SERVICE_STATUS.fromOrdinal(msg.what);
			Log.d(TAG,"New status from CCNx Services: " + st.name());
			// This is very very lazy.  Instead of checking what we got, we'll just
			// update the state and let that get our new status
			updateState();
		}
	};
	
	CCNxServiceCallback cb = new CCNxServiceCallback(){
		public void newCCNxStatus(SERVICE_STATUS st) {
			_handler.sendEmptyMessage(st.ordinal());
		}
	};
	
    @Override
    public void onCreate(Bundle savedInstanceState) {
        super.onCreate(savedInstanceState);
        setContentView(R.layout.controllermain);   
        
        Log.d(TAG,"Creating Service Controller");
        
        _ctx = this.getApplicationContext();
        
        allBtn = (Button)findViewById(R.id.allStartButton);
        allBtn.setOnClickListener(this);
        tvCcndStatus = (TextView)findViewById(R.id.tvCcndStatus);
        tvRepoStatus = (TextView)findViewById(R.id.tvRepoStatus);
        
        init();
    }
    
    @Override
    public void onDestroy() {
    	control.disconnect();
    	super.onDestroy();
    }
    
    private void init(){
    	control = new CCNxServiceControl(this);
    	control.registerCallback(cb);
    	control.connect();
    	updateState();
    }

	public void onClick(View v) {
		switch( v.getId() ) {
		case R.id.allStartButton:
			allButton();
			break;
		default:
			Log.e(TAG, "");
		}
	}

	private void updateState(){
		if(control.allReady()){
			allBtn.setText(R.string.allStopButton);
		} else {
			allBtn.setText(R.string.allStartButton);
		}
		tvCcndStatus.setText(control.getCcndStatus().name());
		tvRepoStatus.setText(control.getRepoStatus().name());
	}
	
	/**
	 * Start all services in the background
	 */
	private void allButton(){
		if(control.allReady()){
			// Everything is ready, we must stop
			control.stoptAll();
		} else {
			// Things are not ready... start them
			control.startAllInBackground();
		}
		updateState();
	}
}
